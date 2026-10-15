#include "usbdirect/engine_pump.h"

#include "usbdirect/transport_core.h"

#include <sched.h>

#include <cerrno>
#include <chrono>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifdef USBDIRECT_USE_EVENTFD
#include <poll.h>
#include <sys/eventfd.h>
#endif

namespace usbdirect {

namespace {
constexpr int kWaitMs = 5; /* wake at least this often even if no signal arrives */
}

EnginePump::EnginePump(UsbAudioOut &out, RenderFn render, unsigned framesPerBlock, size_t targetFillFrames,
                       uint32_t cpuMask)
    : out_(out),
      render_(std::move(render)),
      block_(framesPerBlock ? framesPerBlock : 256),
      target_(targetFillFrames),
      cpuMask_(cpuMask) {
#ifdef USBDIRECT_USE_EVENTFD
    efd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#endif
}

EnginePump::~EnginePump() {
    stop();
#ifdef USBDIRECT_USE_EVENTFD
    if (efd_ >= 0) close(efd_);
#endif
}

void EnginePump::lowWaterThunk(void *self) { static_cast<EnginePump *>(self)->signal(); }

/* Called on the USB event thread: cheap, allocation-free, never blocks. */
void EnginePump::signal() {
#ifdef USBDIRECT_USE_EVENTFD
    const uint64_t one = 1;
    ssize_t r = write(efd_, &one, sizeof(one));
    (void)r; /* EAGAIN means a signal is already pending */
#else
    signaled_.store(true, std::memory_order_release);
    cv_.notify_one(); /* a missed wake-up is bounded by the wait timeout */
#endif
}

void EnginePump::waitForSignal() {
#ifdef USBDIRECT_USE_EVENTFD
    pollfd p{efd_, POLLIN, 0};
    if (poll(&p, 1, kWaitMs) > 0) {
        uint64_t v;
        ssize_t r = read(efd_, &v, sizeof(v));
        (void)r;
    }
#else
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(kWaitMs),
                 [this] { return signaled_.load(std::memory_order_acquire) || quit_.load(); });
    signaled_.store(false, std::memory_order_relaxed);
#endif
}

bool EnginePump::start() {
    if (running_.load()) return false;
    const StreamFormat &f = out_.format();
    if (!f.valid() || f.channels != out_.fifo().channels() || block_ > out_.fifo().capacity()) return false;
    if (!out_.setLowWaterCallback(&EnginePump::lowWaterThunk, this, target_)) return false;
    buf_.assign((size_t)block_ * f.channels, 0.0f);
    quit_.store(false);
    running_.store(true);
    fillToTarget(); /* prefill on the caller's thread so out.start() finds data */
    thread_ = std::thread([this] { threadMain(); });
    return true;
}

void EnginePump::stop() {
    if (!running_.load()) return;
    quit_.store(true);
    signal();
    if (threadId_.load() == std::this_thread::get_id()) return; /* from render: request only, no self-join */
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    out_.setLowWaterCallback(nullptr, nullptr, 0); /* refused once the transport runs; harmless then */
}

uint64_t EnginePump::latencyFrames() const {
    const Stats s = out_.stats();
    return s.usbQueuedFrames + out_.fifo().size();
}

void EnginePump::fillToTarget() {
    const uint64_t blockNs = (uint64_t)block_ * 1000000000ull / out_.format().sampleRate;
    SpscFloatFifo &fifo = out_.fifo();
    while (!quit_.load(std::memory_order_relaxed) && fifo.size() < target_ && fifo.writable() >= block_) {
        const auto t0 = std::chrono::steady_clock::now();
        render_(buf_.data(), (int)block_);
        const auto dt = std::chrono::steady_clock::now() - t0;
        if ((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count() > blockNs)
            overruns_.fetch_add(1, std::memory_order_relaxed);
        fifo.push(buf_.data(), block_); /* room was checked; this producer is the only writer */
        blocks_.fetch_add(1, std::memory_order_relaxed);
    }
}

void EnginePump::threadMain() {
    threadId_.store(std::this_thread::get_id());
#ifdef __ANDROID__
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), -19); /* URGENT_AUDIO, same approach as the event thread */
#endif
#ifdef __linux__
    if (cpuMask_) {
        if (!t7_affinity_valid((long long)cpuMask_)) {
            affinityRc_.store(-1);
        } else {
            cpu_set_t set;
            CPU_ZERO(&set);
            for (int i = 0; i < 8; i++)
                if (t7_mask_has_cpu(cpuMask_, i)) CPU_SET(i, &set);
            affinityRc_.store(sched_setaffinity(0, sizeof(set), &set) == 0 ? 0 : errno);
        }
    }
#endif
    while (!quit_.load(std::memory_order_relaxed)) {
        waitForSignal();
        fillToTarget();
    }
}

} // namespace usbdirect

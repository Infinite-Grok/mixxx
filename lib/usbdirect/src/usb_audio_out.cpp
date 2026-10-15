#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* sched_setaffinity/sched_getcpu on Android */
#endif
#include "usbdirect/usb_audio_out.h"

#include <libusb.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#ifdef __ANDROID__
#include <android/api-level.h>
#include <dlfcn.h>
#endif

#include "usbdirect/depth_controller.h"
#include "usbdirect/device_profile.h"
#include "usbdirect/transport_core.h"

namespace usbdirect {

const char *openResultName(OpenResult r) {
    static const char *const n[] = {"OK", "BAD_ARGS", "ALREADY_OPEN", "UNSUPPORTED_DEVICE", "INIT_FAILED",
                                    "WRAP_FAILED", "SPEED_REJECTED", "CLAIM_FAILED", "ALT1_FAILED",
                                    "ALLOCATION_FAILED", "STOP_REQUESTED"};
    return (unsigned)r < sizeof(n) / sizeof(n[0]) ? n[(unsigned)r] : "UNKNOWN";
}
const char *haltName(Halt h) {
    static const char *const n[] = {"NONE", "USER_STOP", "FEEDBACK_STARTUP_TIMEOUT", "FEEDBACK_STALE",
                                    "INVALID_FEEDBACK", "NO_DEVICE", "TRANSFER_OR_PACKET_ERROR",
                                    "OUT_ACTUAL_LENGTH_MISMATCH", "PACKET_CAPACITY", "SUBMIT_FAILED",
                                    "EVENTS_FAILED", "ALLOCATION_FAILED", "PARTIAL_START_FAILED"};
    return (unsigned)h < sizeof(n) / sizeof(n[0]) ? n[(unsigned)h] : "UNKNOWN";
}

namespace {

constexpr unsigned kMaxOut = 16, kFbDepth = 2, kSlots = kMaxOut + kFbDepth, kPacketBytes = 540;
constexpr unsigned kMaxPacketFrames = kPacketBytes / 12;

uint64_t monoNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Published counters: all uint64_t so the block can be copied through a fixed array. */
struct Raw {
    uint64_t state, reason, depth, depthChanges, depthReason, fbRate, fbValid, fbZero, fbInvalid;
    uint64_t underruns, underrunFrames, submitted, completedOk, outCompleted, errors, framesSubmitted,
        framesCompleted;
    uint64_t outLive, totalLive, gapTotal, gapExceed, gapMax, hist[6], ncand, candT[T7_CAND_MAX],
        candGap[T7_CAND_MAX], missing, eventErrors, watchdog, prioRc, affRc, adpfState, adpfReports, adpfErrors;
};
constexpr size_t kRawN = sizeof(Raw) / sizeof(uint64_t);
static_assert(sizeof(Raw) % sizeof(uint64_t) == 0, "Raw must be a packed uint64_t block");

#ifdef __ANDROID__
int osAffinity(uint64_t req) {
    if (!req) return 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < 64 && i < CPU_SETSIZE; i++)
        if (t7_mask_has_cpu(req, i)) CPU_SET(i, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0 ? 0 : errno;
}
int osApiLevel() { return android_get_device_api_level(); }
#else
int osAffinity(uint64_t) { return 0; }
int osApiLevel() { return -1; }
#endif

struct Adpf {
    void *session = nullptr;
    int (*report)(void *, int64_t) = nullptr;
    void (*close)(void *) = nullptr;
};
#ifdef __ANDROID__
int osAdpfOpen(Adpf *a) {
    *a = Adpf();
    void *lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return T7_ADPF_UNAVAILABLE;
    auto getManager = (void *(*)(void))dlsym(lib, "APerformanceHint_getManager");
    auto create = (void *(*)(void *, const int32_t *, size_t, int64_t))dlsym(lib, "APerformanceHint_createSession");
    a->report = (int (*)(void *, int64_t))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    a->close = (void (*)(void *))dlsym(lib, "APerformanceHint_closeSession");
    if (!getManager || !create || !a->report || !a->close) {
        *a = Adpf();
        return T7_ADPF_UNAVAILABLE;
    }
    void *mgr = getManager();
    if (!mgr) {
        *a = Adpf();
        return T7_ADPF_UNAVAILABLE;
    }
    int32_t tid = (int32_t)syscall(SYS_gettid);
    a->session = create(mgr, &tid, 1, 1000000);
    if (!a->session) {
        *a = Adpf();
        return T7_ADPF_FAILED;
    }
    return T7_ADPF_OK;
}
int osSetPriority() { return setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), -19) == 0 ? 0 : errno; }
#else
int osAdpfOpen(Adpf *) { return T7_ADPF_UNAVAILABLE; }
int osSetPriority() { return 0; }
#endif

} // namespace

struct UsbAudioOut::Impl {
    struct Slot {
        Impl *impl;
        libusb_transfer *t;
        unsigned id;
        unsigned frames; /* frames of the packet currently/last submitted (OUT slots) */
        bool live;
        unsigned char buf[kPacketBytes];
    };

    explicit Impl(const Options &o)
        : opt(o), clk(o.clock ? o.clock : monoNs), fifo(o.fifoFrames),
          ctrl(o.startDepth < 4 ? 4 : o.startDepth > 16 ? 16 : o.startDepth) {
        clock.rate = T7_NOMINAL;
        for (unsigned i = 0; i < kSlots; i++) {
            slots[i].impl = this;
            slots[i].id = i;
        }
        for (auto &a : pub) a.store(0, std::memory_order_relaxed);
    }

    Options opt;
    uint64_t (*clk)();
    SpscFloatFifo fifo;
    const DeviceProfile *profile = nullptr;
    StreamFormat fmt;
    DepthController ctrl;

    libusb_context *ctx = nullptr;
    libusb_device_handle *handle = nullptr;
    T7Clock clock{};
    T7Life life{};
    T7Gaps gaps{};
    Slot slots[kSlots]{};
    unsigned parked[kMaxOut]{};
    unsigned nParked = 0, outLive = 0;
    std::atomic<int> state{(int)State::Closed};
    std::atomic<bool> stopRequested{false};
    std::thread thread;
    std::atomic<int> eventTid{0}; /* kernel tid of the event thread once its priority/affinity are applied */
    Halt reason = Halt::None;
    bool claimed = false, detached = false, alt1 = false, watchdog = false;
    uint64_t startNs = 0, lastOutNs = 0;
    int ownership[256]{};
    int ownershipN = 0;
    Raw w{};
    std::atomic<uint64_t> version{0};
    std::atomic<uint64_t> pub[kRawN];
    void (*lwFn)(void *) = nullptr;
    void *lwUser = nullptr;
    size_t lwFrames = 0;
    std::atomic<bool> inLw{false};
    std::atomic<std::thread::id> lwThread{};
    Adpf adpf;
    T7AdpfLimiter adpfLimiter{};

    /* ---- stats publication (seqlock; single writer at a time) ---- */
    void publish() {
        w.state = (uint64_t)state.load(std::memory_order_relaxed);
        w.reason = (uint64_t)reason;
        w.depth = ctrl.depth();
        w.depthChanges = ctrl.changeCount();
        w.depthReason = (uint64_t)ctrl.lastReason();
        w.fbRate = clock.rate;
        w.fbValid = clock.valid;
        w.outLive = outLive;
        w.totalLive = life.live;
        w.gapTotal = gaps.total;
        w.gapExceed = gaps.exceed;
        for (unsigned i = 0; i < 6; i++) w.hist[i] = gaps.hist[i];
        w.ncand = gaps.ncand;
        for (unsigned i = 0; i < T7_CAND_MAX; i++) {
            w.candT[i] = gaps.cand_t[i];
            w.candGap[i] = gaps.cand_gap[i];
        }
        w.watchdog = watchdog;
        uint64_t arr[kRawN];
        std::memcpy(arr, &w, sizeof(w));
        const uint64_t v = version.load(std::memory_order_relaxed);
        version.store(v + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < kRawN; i++) pub[i].store(arr[i], std::memory_order_relaxed);
        version.store(v + 2, std::memory_order_release);
    }
    Raw readRaw() const {
        uint64_t arr[kRawN];
        for (;;) {
            const uint64_t v1 = version.load(std::memory_order_acquire);
            if (v1 & 1) continue;
            for (size_t i = 0; i < kRawN; i++) arr[i] = pub[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (version.load(std::memory_order_relaxed) == v1) break;
        }
        Raw r;
        std::memcpy(&r, arr, sizeof(r));
        return r;
    }

    void note(int tag, int rc) {
        if (ownershipN < 254) {
            ownership[ownershipN++] = tag;
            ownership[ownershipN++] = rc;
        }
    }
    void halt(Halt r) {
        if (!life.stopping) {
            reason = r;
            t7_stop(&life, clk());
            state.store((int)State::Stopping, std::memory_order_relaxed);
        }
    }

    /* ---- submission (callback-safe: no allocation, logging or locks) ---- */
    int submitFeedback(Slot *p) {
        if (!t7_may_submit(&life)) return -1;
        libusb_fill_iso_transfer(p->t, handle, 0x81, p->buf, 3, 1, &Impl::onComplete, p, 0);
        libusb_set_iso_packet_lengths(p->t, 3);
        return finishSubmit(p);
    }
    /* Each OUT submission takes the next 1 ms packet of the stream: frame count from the
     * feedback clock, samples from the FIFO. Because every packet - first, replacement,
     * raise-extra - is produced here, in submission order, depth changes cannot create a
     * sequence gap or duplicate. */
    int submitOut(Slot *p) {
        if (!t7_may_submit(&life)) return -1;
        const int frames = t7_frames(&clock, kPacketBytes);
        if (frames < 0) {
            halt(Halt::PacketCapacity);
            return -1;
        }
        float tmp[kMaxPacketFrames * SpscFloatFifo::kChannels];
        const unsigned n = (unsigned)frames;
        const size_t got = n ? fifo.pop(tmp, n) : 0;
        if (got < n) { /* engine shortfall: keep what exists, zero-fill the rest */
            std::memset(tmp + got * SpscFloatFifo::kChannels, 0, (n - got) * SpscFloatFifo::kChannels * sizeof(float));
            w.underruns++;
            w.underrunFrames += n - got;
        }
        packFrames(p->buf, tmp, n, fmt.channels, fmt.sampleFormat);
        const unsigned length = n * fmt.bytesPerFrame;
        libusb_fill_iso_transfer(p->t, handle, 0x01, p->buf, (int)length, 1, &Impl::onComplete, p, 0);
        libusb_set_iso_packet_lengths(p->t, length);
        const int rc = finishSubmit(p);
        if (rc == 0) {
            outLive++;
            w.submitted++;
            w.framesSubmitted += n;
            p->frames = n;
        }
        return rc;
    }
    int finishSubmit(Slot *p) {
        const int rc = libusb_submit_transfer(p->t);
        if (rc) {
            halt(rc == LIBUSB_ERROR_NO_DEVICE ? Halt::NoDevice : Halt::SubmitFailed);
            return rc;
        }
        p->live = true;
        t7_submitted(&life);
        if (!startNs) {
            startNs = clk();
            clock.first_ns = startNs;
        }
        return 0;
    }

    static void LIBUSB_CALL onComplete(libusb_transfer *t) {
        Slot *p = static_cast<Slot *>(t->user_data);
        if (p->id < kMaxOut) p->impl->outDone(p);
        else p->impl->feedbackDone(p);
    }

    void common(Slot *p) {
        p->live = false;
        t7_reaped(&life);
        if (stopRequested.load(std::memory_order_relaxed)) halt(Halt::UserStop);
    }
    void clockCheck(uint64_t now) {
        const int e = t7_clock_error(&clock, now);
        if (e) halt(e == 1 ? Halt::FeedbackStartupTimeout : Halt::FeedbackStale);
    }

    void feedbackDone(Slot *p) {
        const uint64_t now = clk();
        libusb_transfer *t = p->t;
        const libusb_iso_packet_descriptor *d = &t->iso_packet_desc[0];
        common(p);
        const bool ok = t->status == LIBUSB_TRANSFER_COMPLETED && d->status == LIBUSB_TRANSFER_COMPLETED;
        if (ok) {
            T7Clock observed = clock;
            const int r = t7_feedback(&observed, p->buf, d->actual_length, now);
            if (r == 0) w.fbZero++;
            if (r < 0) {
                w.fbInvalid++;
                w.errors++;
            }
        } else if (t->status != LIBUSB_TRANSFER_CANCELLED && d->status != LIBUSB_TRANSFER_CANCELLED) {
            w.errors++;
        }
        if (!life.stopping) clockCheck(now);
        if (!life.stopping && !ok)
            halt(t->status == LIBUSB_TRANSFER_NO_DEVICE ? Halt::NoDevice : Halt::TransferOrPacketError);
        if (!life.stopping && t7_feedback(&clock, p->buf, d->actual_length, now) < 0) halt(Halt::InvalidFeedback);
        if (!life.stopping) clockCheck(now);
        if (!life.stopping) submitFeedback(p);
        publish();
    }

    void parkSlot(Slot *p) { parked[nParked++] = p->id; }

    void outDone(Slot *p) {
        const uint64_t now = clk();
        libusb_transfer *t = p->t;
        const libusb_iso_packet_descriptor *d = &t->iso_packet_desc[0];
        common(p);
        outLive--;
        w.outCompleted++;
        w.framesCompleted += p->frames;
        const bool ok = t->status == LIBUSB_TRANSFER_COMPLETED && d->status == LIBUSB_TRANSFER_COMPLETED;
        if (ok && d->actual_length == d->length) w.completedOk++;
        if ((!ok && t->status != LIBUSB_TRANSFER_CANCELLED && d->status != LIBUSB_TRANSFER_CANCELLED) ||
            (ok && d->actual_length != d->length))
            w.errors++;
        parkSlot(p);
        if (!life.stopping) clockCheck(now);
        if (!life.stopping && !ok)
            halt(t->status == LIBUSB_TRANSFER_NO_DEVICE ? Halt::NoDevice : Halt::TransferOrPacketError);
        if (!life.stopping && d->actual_length != d->length) halt(Halt::OutActualLengthMismatch);
        if (life.stopping) {
            publish();
            return;
        }
        if (lastOutNs) {
            const uint64_t gap = now - lastOutNs;
            if (gap > w.gapMax) w.gapMax = gap;
            t7_gap_record(&gaps, ctrl.depth(), gap, now - startNs);
            ctrl.onGap(gap, now);
        }
        lastOutNs = now;
        if (outLive == 0) { /* no other OUT transfer queued: the queue ran dry */
            w.missing++;
            ctrl.onMissingSlot(now);
        }
        /* Depth change without a packet gap: raise = this callback submits extra packets from
         * parked slots; lower = this slot is simply not resubmitted (retired). Either way the
         * next packet always comes from the single ordered producer in submitOut(). */
        const unsigned want = ctrl.depth();
        while (outLive < want && nParked) {
            Slot *s = &slots[parked[--nParked]];
            if (submitOut(s)) {
                parkSlot(s);
                break;
            }
        }
        publish();
    }

    /* ---- event loop / lifecycle (not callback context) ---- */
    void eventStepImpl() {
        const uint64_t now = clk();
        if (stopRequested.load(std::memory_order_relaxed)) halt(Halt::UserStop);
        clockCheck(now);
        if (life.stopping) {
            publish();
            return;
        }
        timeval tv{0, 5000};
        const int rc = libusb_handle_events_timeout(ctx, &tv);
        if (rc && rc != LIBUSB_ERROR_INTERRUPTED) {
            w.eventErrors++;
            note(22, rc);
            halt(Halt::EventsFailed);
        }
        if (lwFn && fifo.size() < lwFrames) {
            lwThread.store(std::this_thread::get_id());
            inLw.store(true);
            lwFn(lwUser);
            inLw.store(false);
        }
        if (adpf.session) {
            const uint64_t t = clk();
            if (t7_adpf_due(&adpfLimiter, t)) {
                w.adpfReports++;
                if (adpf.report(adpf.session, (int64_t)t7_adpf_clamp(t - now))) w.adpfErrors++;
            }
        }
        publish();
    }
    void threadMain() {
        if (opt.setPriority) w.prioRc = (uint64_t)osSetPriority();
        w.affRc = (uint64_t)osAffinity(t7_affinity_valid((long long)opt.cpuMask) ? opt.cpuMask : 0);
        eventTid.store((int)syscall(SYS_gettid), std::memory_order_release);
        w.adpfState = T7_ADPF_UNAVAILABLE;
        if (opt.adpf && t7_adpf_api_ok(osApiLevel())) w.adpfState = (uint64_t)osAdpfOpen(&adpf);
        while (!life.stopping) eventStepImpl();
        if (adpf.session) {
            adpf.close(adpf.session);
            adpf.session = nullptr;
        }
        publish();
    }

    void drain() {
        for (unsigned i = 0; i < kSlots; i++)
            if (slots[i].live) note(21, libusb_cancel_transfer(slots[i].t));
        while (life.live) {
            if (t7_pending(&life, clk()) && !watchdog) {
                watchdog = true; /* owner keeps fd, transfers, buffers and context; never frees live */
                publish();
            }
            timeval tv{0, 5000};
            const int rc = libusb_handle_events_timeout(ctx, &tv);
            if (rc && rc != LIBUSB_ERROR_INTERRUPTED) {
                if (!w.eventErrors) note(22, rc);
                w.eventErrors++;
            }
        }
    }
    void cleanup() {
        for (unsigned i = 0; i < kSlots; i++)
            if (slots[i].t) {
                libusb_free_transfer(slots[i].t);
                slots[i].t = nullptr;
            }
        if (claimed) note(7, libusb_set_interface_alt_setting(handle, 1, 0));
        if (claimed) note(8, libusb_release_interface(handle, 1));
        if (detached) note(9, libusb_attach_kernel_driver(handle, 1));
        detachedEver = detachedEver || detached;
        claimed = detached = alt1 = false;
        if (handle) {
            for (int i = 0; i < 4; i++) note(14 + i, libusb_kernel_driver_active(handle, i));
            libusb_close(handle);
            handle = nullptr;
        }
        if (ctx) {
            libusb_exit(ctx);
            ctx = nullptr;
        }
    }
    bool detachedEver = false;

    OpenResult openImpl(int fd, const unsigned char *raw, size_t len) {
        if (state.load() != (int)State::Closed) return OpenResult::AlreadyOpen;
        if (fd < 0 || !raw) return OpenResult::BadArgs;
        profile = findProfile(raw, len);
        if (!profile || profile->transport != TransportVariant::AsyncExplicitFeedbackFS ||
            profile->format.channels != SpscFloatFifo::kChannels || profile->format.bytesPerFrame * 45u > kPacketBytes) {
            profile = nullptr;
            return OpenResult::UnsupportedDevice;
        }
        fmt = profile->format;
        OpenResult res = OpenResult::Ok;
        int rc = 0, driver1 = 0;
        libusb_init_option option{};
        option.option = LIBUSB_OPTION_NO_DEVICE_DISCOVERY;
        rc = libusb_init_context(&ctx, &option, 1);
        note(1, rc);
        if (rc) { res = OpenResult::InitFailed; goto fail; }
        rc = libusb_wrap_sys_device(ctx, (intptr_t)fd, &handle);
        note(2, rc);
        if (rc) { res = OpenResult::WrapFailed; goto fail; }
        if (libusb_get_device_speed(libusb_get_device(handle)) != LIBUSB_SPEED_FULL) {
            res = OpenResult::SpeedRejected;
            goto fail;
        }
        for (int i = 0; i < 4; i++) { /* read-only driver status; only IF1 may be claimed */
            const int active = libusb_kernel_driver_active(handle, i);
            note(10 + i, active);
            if (i == 1) driver1 = active;
        }
        if (stopRequested.load()) { res = OpenResult::StopRequested; goto fail; }
        rc = libusb_claim_interface(handle, 1);
        note(3, rc);
        if (rc == LIBUSB_ERROR_BUSY && driver1 == 1) { /* single detach + single retry */
            rc = libusb_detach_kernel_driver(handle, 1);
            note(4, rc);
            if (!rc) {
                detached = true;
                rc = libusb_claim_interface(handle, 1);
                note(5, rc);
            }
        }
        if (rc) { res = OpenResult::ClaimFailed; goto fail; }
        claimed = true;
        rc = libusb_set_interface_alt_setting(handle, 1, 1);
        note(6, rc);
        if (rc) { res = OpenResult::Alt1Failed; goto fail; }
        alt1 = true;
        for (unsigned i = 0; i < kSlots; i++) {
            slots[i].t = libusb_alloc_transfer(1);
            if (!slots[i].t) { res = OpenResult::AllocFailed; goto fail; }
        }
        nParked = 0;
        for (unsigned i = kMaxOut; i-- > 0;) parked[nParked++] = i; /* pops 0,1,2,... first */
        state.store((int)State::Open);
        publish();
        return OpenResult::Ok;
    fail:
        profile = nullptr;
        fmt = StreamFormat();
        cleanup();
        publish();
        return res;
    }

    bool startImpl() {
        if (state.load() != (int)State::Open) return false;
        state.store((int)State::Running);
        stopRequested.store(false);
        bool ok = true;
        for (unsigned i = kMaxOut; i < kSlots && ok; i++) ok = submitFeedback(&slots[i]) == 0;
        for (unsigned i = 0; i < ctrl.depth() && ok && nParked; i++) {
            Slot *s = &slots[parked[--nParked]];
            if (submitOut(s)) {
                parkSlot(s);
                ok = false;
            }
        }
        if (!ok) {
            halt(Halt::PartialStartFailed);
            publish();
            return false;
        }
        publish();
        if (opt.spawnThread) thread = std::thread([this] { threadMain(); });
        return true;
    }

    void stopImpl() {
        if (inLw.load() && lwThread.load() == std::this_thread::get_id()) {
            stopRequested.store(true); /* from the low-water callback: never self-join; owner stops again */
            return;
        }
        const int st = state.load();
        if (st == (int)State::Closed || st == (int)State::Stopped) return;
        stopRequested.store(true);
        if (thread.joinable()) thread.join();
        eventTid.store(0, std::memory_order_release);
        if (st != (int)State::Open) {
            halt(Halt::UserStop);
            drain();
        }
        cleanup();
        state.store((int)State::Stopped);
        publish();
    }
};

UsbAudioOut::UsbAudioOut(const Options &opt) : impl_(new Impl(opt)) {}
UsbAudioOut::~UsbAudioOut() { impl_->stopImpl(); }
OpenResult UsbAudioOut::open(int fd, const unsigned char *raw, size_t len) { return impl_->openImpl(fd, raw, len); }
bool UsbAudioOut::start() { return impl_->startImpl(); }
void UsbAudioOut::stop() { impl_->stopImpl(); }
SpscFloatFifo &UsbAudioOut::fifo() { return impl_->fifo; }
bool UsbAudioOut::setLowWaterCallback(void (*fn)(void *), void *user, size_t frames) {
    const int st = impl_->state.load();
    if (st != (int)State::Closed && st != (int)State::Open) return false; /* the event thread reads these */
    impl_->lwFn = fn;
    impl_->lwUser = user;
    impl_->lwFrames = frames;
    return true;
}
const StreamFormat &UsbAudioOut::format() const { return impl_->fmt; }
void UsbAudioOut::eventStep() { impl_->eventStepImpl(); }
int UsbAudioOut::ownershipCount() const { return impl_->ownershipN / 2; }
int UsbAudioOut::ownershipTag(int i) const { return impl_->ownership[2 * i]; }
int UsbAudioOut::ownershipReturn(int i) const { return impl_->ownership[2 * i + 1]; }
bool UsbAudioOut::detachedKernelDriver() const { return impl_->detachedEver || impl_->detached; }

int UsbAudioOut::eventThreadTid() const { return impl_->eventTid.load(std::memory_order_acquire); }
Stats UsbAudioOut::stats() const {
    const Raw r = impl_->readRaw();
    Stats s;
    s.state = (State)r.state;
    s.reason = (Halt)r.reason;
    s.depth = (unsigned)r.depth;
    s.depthChanges = (unsigned)r.depthChanges;
    s.depthReason = (int)r.depthReason;
    s.feedbackRate = (uint32_t)r.fbRate;
    s.feedbackHz = (double)r.fbRate * 1000.0 / (double)T7_DENOM;
    s.feedbackValid = r.fbValid;
    s.feedbackZero = r.fbZero;
    s.feedbackInvalid = r.fbInvalid;
    s.fifoFill = impl_->fifo.size();
    s.fifoCapacity = impl_->fifo.capacity();
    s.engineUnderruns = r.underruns;
    s.engineUnderrunFrames = r.underrunFrames;
    s.packetsSubmitted = r.submitted;
    s.packetsCompletedOk = r.completedOk;
    s.outCompleted = r.outCompleted;
    s.errors = r.errors;
    s.framesSubmitted = r.framesSubmitted;
    s.framesCompleted = r.framesCompleted;
    s.usbQueuedFrames = r.framesSubmitted - r.framesCompleted;
    s.outLive = (unsigned)r.outLive;
    s.totalLive = (unsigned)r.totalLive;
    s.gapTotal = r.gapTotal;
    s.gapExceedDepth = r.gapExceed;
    s.gapMaxNs = r.gapMax;
    for (unsigned i = 0; i < 6; i++) s.gapHist[i] = r.hist[i];
    s.candidates = (unsigned)r.ncand;
    for (unsigned i = 0; i < T7_CAND_MAX; i++) {
        s.candidateT[i] = r.candT[i];
        s.candidateGap[i] = r.candGap[i];
    }
    s.missingSlotEvents = r.missing;
    s.eventErrors = r.eventErrors;
    s.drainWatchdog = r.watchdog != 0;
    s.priorityRc = (int)r.prioRc;
    s.affinityRc = (int)r.affRc;
    s.adpfState = (int)r.adpfState;
    s.adpfReports = r.adpfReports;
    s.adpfErrors = r.adpfErrors;
    return s;
}

} // namespace usbdirect

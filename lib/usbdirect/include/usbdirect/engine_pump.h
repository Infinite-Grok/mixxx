#ifndef USBDIRECT_ENGINE_PUMP_H
#define USBDIRECT_ENGINE_PUMP_H
/* EnginePump: owns the engine thread that renders audio blocks into UsbAudioOut's FIFO.
 *
 * Order of use:   pump.start();  out.start();   ...   out.stop();  pump.stop();
 * start() wires UsbAudioOut::setLowWaterCallback (so it must run before out.start(); the call is
 * refused once the transport runs), prefills the FIFO to the target and starts the thread.
 *
 * Threading: the engine thread is the only FIFO producer. `render` runs on it, never on the USB
 * event thread; it may block briefly but a render slower than the block duration counts as an
 * overrun (the USB side then zero-fills, see Stats::engineUnderruns). stop() must not be called
 * from `render` or from the low-water callback (from the render thread it is only a request and
 * does not join; call it again from another thread). The pump must outlive the running transport:
 * stop the UsbAudioOut first (its event thread may still call the low-water thunk). */
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "usb_audio_out.h"

#if defined(__ANDROID__) && !defined(USBDIRECT_USE_EVENTFD)
#define USBDIRECT_USE_EVENTFD 1 /* host default: condition variable; Android: eventfd */
#endif

namespace usbdirect {

class EnginePump {
public:
    /* render(interleaved, frames): fill `frames` frames of format().channels interleaved floats. */
    using RenderFn = std::function<void(float *interleaved, int frames)>;

    EnginePump(UsbAudioOut &out, RenderFn render, unsigned framesPerBlock, size_t targetFillFrames);
    ~EnginePump();
    EnginePump(const EnginePump &) = delete;
    EnginePump &operator=(const EnginePump &) = delete;

    /* Requires out.open() to have succeeded and the transport not yet started. Returns false (and
     * starts nothing) otherwise, or if the FIFO channel count differs from format().channels. */
    bool start();
    /* Idempotent; joins the engine thread. */
    void stop();

    /* usbQueuedFrames + fifoFill: frames between render() and the wire. Fill and queue are read
     * back-to-back, so the value can be off by at most one packet (45 frames) at an instant. */
    uint64_t latencyFrames() const;
    /* Blocks whose render() took longer than the block duration. */
    uint64_t renderOverruns() const { return overruns_.load(std::memory_order_relaxed); }
    uint64_t blocksRendered() const { return blocks_.load(std::memory_order_relaxed); }

private:
    static void lowWaterThunk(void *self);
    void signal();
    void waitForSignal();
    void threadMain();
    void fillToTarget();

    UsbAudioOut &out_;
    RenderFn render_;
    unsigned block_;
    size_t target_;
    std::vector<float> buf_; /* preallocated: block_ * channels */
    std::atomic<bool> quit_{false}, running_{false};
    std::atomic<uint64_t> overruns_{0}, blocks_{0};
    std::thread thread_;
    std::atomic<std::thread::id> threadId_{};
#ifdef USBDIRECT_USE_EVENTFD
    int efd_ = -1;
#else
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> signaled_{false};
#endif
};

} // namespace usbdirect
#endif

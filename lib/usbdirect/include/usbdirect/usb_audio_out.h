#ifndef USBDIRECT_USB_AUDIO_OUT_H
#define USBDIRECT_USB_AUDIO_OUT_H
#include <cstddef>
#include <cstdint>
#include <memory>

#include "device_profile.h"
#include "spsc_float_fifo.h"
#include "stream_format.h"

namespace usbdirect {

enum class OpenResult {
    Ok = 0, BadArgs, AlreadyOpen, UnsupportedDevice, InitFailed, WrapFailed, SpeedRejected,
    ClaimFailed, Alt1Failed, AllocFailed, StopRequested,
    DescriptorMismatch = UnsupportedDevice /* I1 name, kept as an alias */
};
const char *openResultName(OpenResult r);

enum class State { Closed = 0, Open = 1, Running = 2, Stopping = 3, Stopped = 4 };

/* Why streaming halted (first cause wins). */
enum class Halt {
    None = 0, UserStop, FeedbackStartupTimeout, FeedbackStale, InvalidFeedback, NoDevice,
    TransferOrPacketError, OutActualLengthMismatch, PacketCapacity, SubmitFailed, EventsFailed,
    AllocationFailed, PartialStartFailed
};
const char *haltName(Halt h);

struct Options {
    unsigned startDepth = 12;      /* initial OUT queue depth (1 ms packets), clamped [minDepth,16] */
    unsigned minDepth = 4;         /* depth floor, clamped [2,16] */
    unsigned depthWindowMs = 120000; /* lower-step window, clamped [5000,600000] */
    size_t fifoFrames = 16384;     /* FIFO capacity, power of two >= 8192 (rounded up) */
    bool spawnThread = true;       /* false: caller drives eventStep() (host tests) */
    bool setPriority = true;       /* nice -19 on the event thread (Android only) */
    uint32_t cpuMask = 0;          /* 0 = no affinity request; else bitmask of CPUs 0..7 */
    bool adpf = false;             /* optional ADPF hint session (Android API 33+, dlopen) */
    uint64_t (*clock)() = nullptr; /* CLOCK_MONOTONIC ns; injectable for tests */
};

/* Consistent snapshot (seqlock-validated) of the counters published by the USB thread. */
struct Stats {
    State state = State::Closed;
    Halt reason = Halt::None;
    unsigned depth = 0;               /* current target OUT depth (packets == ms) */
    unsigned depthChanges = 0;
    int depthReason = 0;              /* DepthController::Reason */
    uint32_t feedbackRate = 0;        /* raw clock rate: frames per 1 ms packet * 163840 */
    double feedbackHz = 0;            /* feedbackRate * 1000 / 163840 (nominal 44100) */
    uint64_t feedbackValid = 0, feedbackZero = 0, feedbackInvalid = 0;
    size_t fifoFill = 0, fifoCapacity = 0;
    uint64_t engineUnderruns = 0;     /* packets that needed zero-fill (engine shortfall) */
    uint64_t engineUnderrunFrames = 0;
    uint64_t packetsSubmitted = 0, packetsCompletedOk = 0, outCompleted = 0, errors = 0;
    uint64_t framesSubmitted = 0, framesCompleted = 0;
    uint64_t usbQueuedFrames = 0;     /* framesSubmitted - framesCompleted (frames in flight on USB) */
    unsigned outLive = 0, totalLive = 0;
    uint64_t gapTotal = 0, gapExceedDepth = 0, gapMaxNs = 0;
    uint64_t gapHist[6] = {0, 0, 0, 0, 0, 0}; /* > 1.5, 2, 3, 4, 5, 8 ms */
    unsigned candidates = 0;          /* USB gap candidates listed (<= 64) */
    uint64_t candidateT[64] = {};
    uint64_t candidateGap[64] = {};
    uint64_t missingSlotEvents = 0, eventErrors = 0;
    bool drainWatchdog = false;
    int priorityRc = 0, affinityRc = 0, adpfState = 0;
    uint64_t adpfReports = 0, adpfErrors = 0;
};

/* Direct isochronous USB audio output for registry devices (today: the exact T7 profile, IF1 alt1, 44100 Hz, 4ch s24le,
 * async feedback on EP 0x81). See README.md for threading and invariants. */
class UsbAudioOut {
public:
    explicit UsbAudioOut(const Options &opt = Options());
    ~UsbAudioOut(); /* stops and releases everything if still open */
    UsbAudioOut(const UsbAudioOut &) = delete;
    UsbAudioOut &operator=(const UsbAudioOut &) = delete;

    /* fd: Android UsbDeviceConnection fd (ownership stays with the caller until stop()
     * completes; it must not be closed earlier). raw/len: raw descriptors, must be byte-
     * identical to a DeviceProfile registry entry; otherwise UnsupportedDevice. No USB call happens
     * before the gate passes. */
    OpenResult open(int fd, const unsigned char *raw, size_t len);
    /* Submits 2 feedback + depth OUT transfers and (if spawnThread) starts the event thread.
     * Prefill fifo() first. Returns false on failure; call stop() afterwards in any case. */
    bool start();
    /* Idempotent. Blocks until every transfer is reaped, then alt0, release IF1, reattach
     * only if this object detached, close, exit. Never resets/sets configuration/rate.
     * Never call from the low-water or render callbacks: from the low-water callback it only
     * requests the stop (no join) and the owner must call stop() again. */
    void stop();

    SpscFloatFifo &fifo();
    Stats stats() const;
    /* Kernel tid of the event thread, 0 until the thread has applied its priority/affinity (and after stop(), or
     * when spawnThread is false). */
    int eventThreadTid() const;
    /* Only valid before start() (Closed or Open state); returns false and changes nothing once
     * running/stopping. fn is called on the event thread (never inside a libusb callback) whenever the FIFO
     * holds fewer than `frames` frames; it must be cheap (e.g. eventfd write). stop() called from
     * fn only requests the stop (never self-joins); the owner must call stop() again afterwards. */
    bool setLowWaterCallback(void (*fn)(void *), void *user, size_t frames);
    /* Stream format of the matched profile; invalid (valid()==false) until open() succeeds. */
    const StreamFormat &format() const;

    /* Event-loop iteration (libusb_handle_events_timeout + housekeeping). Used internally by
     * the event thread; public for spawnThread=false. Not for use while a thread is running. */
    void eventStep();
    /* Cleanup result record (valid after stop()): ownership tags/returns as in the probe. */
    int ownershipCount() const;
    int ownershipTag(int i) const;
    int ownershipReturn(int i) const;
    bool detachedKernelDriver() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace usbdirect
#endif

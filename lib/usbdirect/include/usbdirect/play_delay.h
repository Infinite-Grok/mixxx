#ifndef USBDIRECT_PLAY_DELAY_H
#define USBDIRECT_PLAY_DELAY_H
/* Seconds from "the engine starts rendering a block" to "its first frame reaches the DAC", for VisualPlayPosition.
 *
 * At the entry of render() the block being rendered is not in the FIFO yet (the pump pushes it after render()
 * returns), so the frames ahead of it are exactly the frames still queued on USB plus the FIFO content:
 *   delay = (usbQueuedFrames + fifoFrames) / sampleRate.
 * The USB snapshot can lag the live FIFO by at most one packet (~1 ms); no further constant is added.
 *
 * Validation (same idea as SoundDevicePortAudio::updateCallbackEntryToDacTime): the value must lie in
 * [0, 2 * expectedLatencySecs]. An implausible value is replaced by the previous result advanced to now
 * (previous + one block - time since the previous render), clamped to the same range. Without a previous value the
 * raw value is only clamped. expectedLatencySecs <= 0 or sampleRate <= 0 disables the upper bound / returns 0. */
#include <algorithm>
#include <cstdint>

namespace usbdirect {

struct DacDelayInput {
    uint64_t usbQueuedFrames = 0;
    uint64_t fifoFrames = 0;
    double sampleRate = 0.0;
    double blockFrames = 0.0;
    double expectedLatencySecs = 0.0;
    bool haveLast = false;
    double lastSecs = 0.0;            /* result of the previous call */
    double sinceLastRenderSecs = 0.0; /* CPU time since the previous render() entry */
};

struct DacDelay {
    double secs = 0.0;
    bool substituted = false; /* the raw value was implausible */
};

inline DacDelay dacDelaySecs(const DacDelayInput &in) {
    DacDelay out;
    if (in.sampleRate <= 0.0) return out;
    const double raw = (double)(in.usbQueuedFrames + in.fifoFrames) / in.sampleRate;
    const double maxSecs = in.expectedLatencySecs > 0.0 ? 2.0 * in.expectedLatencySecs : raw;
    if (raw >= 0.0 && raw <= maxSecs) {
        out.secs = raw;
        return out;
    }
    out.substituted = true;
    double v = raw;
    if (in.haveLast) v = in.lastSecs + in.blockFrames / in.sampleRate - in.sinceLastRenderSecs;
    out.secs = std::min(std::max(v, 0.0), maxSecs);
    return out;
}

} // namespace usbdirect
#endif

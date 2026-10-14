#ifndef USBDIRECT_DEPTH_CONTROLLER_H
#define USBDIRECT_DEPTH_CONTROLLER_H
#include <algorithm>
#include <cstdint>

namespace usbdirect {

/* Pure adaptive OUT queue-depth policy (depth in 1 ms packets). Single-threaded: called only
 * from the USB completion path. Integer arithmetic only; no allocation, no logging, no locks.
 *
 * Raise: any OUT completion gap strictly greater than the current depth in ms (underrun
 * candidate, same rule as T7Gaps) or missing-slot evidence ->
 *   depth = min(max, max(depth + 2, ceil(gap_ms) + 2)) immediately.
 * Lower: a window of windowNs (120 s) with no change and at least kMinWindowSamples (10000,
 * enough for a p99.99 estimate) gaps ends -> target = ceil(maxGapInWindow_ms + 1) clamped to
 * [min,max]; if target < depth, depth -= 1 (at most one step per window, never below target).
 * Every depth change (and window end) starts a new window. */
class DepthController {
public:
    enum class Reason { None = 0, GapExceeded = 1, MissingSlot = 2, WindowLower = 3 };
    static constexpr uint64_t kMinWindowSamples = 10000;

    explicit DepthController(unsigned start = 12, unsigned minDepth = 4, unsigned maxDepth = 16,
                             uint64_t windowNs = 120ull * 1000000000ull)
        : min_(minDepth), max_(maxDepth), window_(windowNs) {
        depth_ = std::min(max_, std::max(min_, start));
    }

    unsigned depth() const { return depth_; }
    Reason lastReason() const { return reason_; }
    unsigned changeCount() const { return changes_; }
    unsigned minDepth() const { return min_; }
    unsigned maxDepth() const { return max_; }

    /* One OUT completion gap (ns between consecutive OUT completions). Returns true if the
     * depth changed (caller adjusts the in-flight OUT count). */
    bool onGap(uint64_t gapNs, uint64_t nowNs) {
        if (!windowOpen_) openWindow(nowNs);
        if (gapNs > (uint64_t)depth_ * 1000000ull) {
            const unsigned gapMs = ceilMs(gapNs);
            const bool changed = set(std::max(depth_ + 2, gapMs + 2), Reason::GapExceeded, nowNs);
            if (!changed) openWindow(nowNs); /* at the cap: the window is still not clean */
            return changed;
        }
        if (gapNs > windowMax_) windowMax_ = gapNs;
        windowCount_++;
        if (nowNs - windowStart_ >= window_) {
            const bool known = windowCount_ >= kMinWindowSamples;
            const unsigned target = clampDepth(ceilMs(windowMax_ + 1000000ull));
            const bool lower = known && target < depth_;
            if (lower) return set(depth_ - 1, Reason::WindowLower, nowNs);
            openWindow(nowNs);
        }
        return false;
    }
    /* The queue ran dry (no other OUT transfer in flight at a completion). */
    bool onMissingSlot(uint64_t nowNs) {
        if (!windowOpen_) openWindow(nowNs);
        const bool changed = set(depth_ + 2, Reason::MissingSlot, nowNs);
        if (!changed) openWindow(nowNs);
        return changed;
    }

private:
    static unsigned ceilMs(uint64_t ns) {
        const uint64_t ms = (ns + 999999ull) / 1000000ull;
        return ms > 1000 ? 1000u : (unsigned)ms;
    }
    unsigned clampDepth(unsigned d) const { return std::min(max_, std::max(min_, d)); }
    void openWindow(uint64_t nowNs) {
        windowOpen_ = true;
        windowStart_ = nowNs;
        windowMax_ = 0;
        windowCount_ = 0;
    }
    bool set(unsigned wanted, Reason why, uint64_t nowNs) {
        const unsigned d = clampDepth(wanted);
        if (d == depth_) return false;
        depth_ = d;
        reason_ = why;
        changes_++;
        openWindow(nowNs);
        return true;
    }
    unsigned min_, max_, depth_;
    uint64_t window_;
    Reason reason_ = Reason::None;
    unsigned changes_ = 0;
    bool windowOpen_ = false;
    uint64_t windowStart_ = 0, windowMax_ = 0, windowCount_ = 0;
};

} // namespace usbdirect
#endif

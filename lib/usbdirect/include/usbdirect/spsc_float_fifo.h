#ifndef USBDIRECT_SPSC_FLOAT_FIFO_H
#define USBDIRECT_SPSC_FLOAT_FIFO_H
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace usbdirect {
/* Lock-free single-producer/single-consumer ring of interleaved float frames (4 channels by default,
 * runtime-selectable via the constructor).
 * Producer = engine thread (push), consumer = USB completion callback (pop).
 * Capacity is a power of two >= kMinFrames. Storage is allocated once in the constructor;
 * push/pop never allocate, lock or loop (wait-free). Indices are monotonically increasing
 * 64-bit counters (no ABA/wrap ambiguity); free-running difference gives the fill. */
class SpscFloatFifo {
public:
    static constexpr size_t kChannels = 4;
    static constexpr size_t kMinFrames = 8192;

    /* Capacity is rounded up to a power of two and to at least kMinFrames. */
    explicit SpscFloatFifo(size_t capacityFrames = 16384, size_t channels = kChannels) : ch_(channels ? channels : kChannels) {
        size_t c = kMinFrames;
        while (c < capacityFrames) c <<= 1;
        cap_ = c;
        mask_ = c - 1;
        buf_.reset(new float[c * ch_]());
    }
    SpscFloatFifo(const SpscFloatFifo &) = delete;
    SpscFloatFifo &operator=(const SpscFloatFifo &) = delete;

    size_t capacity() const { return cap_; }
    size_t channels() const { return ch_; }

    /* Producer only. Copies up to n frames; returns frames accepted (< n when full). */
    size_t push(const float *frames, size_t n) {
        const uint64_t w = write_.load(std::memory_order_relaxed);
        const uint64_t r = read_.load(std::memory_order_acquire);
        const size_t room = cap_ - (size_t)(w - r);
        if (n > room) n = room;
        copyIn(w, frames, n);
        write_.store(w + n, std::memory_order_release);
        return n;
    }
    /* Consumer only. Copies up to n frames out; returns frames delivered (< n when empty). */
    size_t pop(float *out, size_t n) {
        const uint64_t r = read_.load(std::memory_order_relaxed);
        const uint64_t w = write_.load(std::memory_order_acquire);
        const size_t avail = (size_t)(w - r);
        if (n > avail) n = avail;
        copyOut(r, out, n);
        read_.store(r + n, std::memory_order_release);
        return n;
    }
    /* Any thread; a racy but always-valid instantaneous value in [0, capacity]. */
    size_t size() const {
        const uint64_t r = read_.load(std::memory_order_acquire);
        const uint64_t w = write_.load(std::memory_order_acquire);
        const uint64_t d = w - r;
        return d > cap_ ? cap_ : (size_t)d;
    }
    size_t writable() const { return cap_ - size(); }
    uint64_t totalPushed() const { return write_.load(std::memory_order_acquire); }
    uint64_t totalPopped() const { return read_.load(std::memory_order_acquire); }

private:
    void copyIn(uint64_t w, const float *src, size_t n) {
        const size_t start = (size_t)(w & mask_);
        const size_t first = n < cap_ - start ? n : cap_ - start;
        std::memcpy(&buf_[start * ch_], src, first * ch_ * sizeof(float));
        if (n > first) std::memcpy(&buf_[0], src + first * ch_, (n - first) * ch_ * sizeof(float));
    }
    void copyOut(uint64_t r, float *dst, size_t n) const {
        const size_t start = (size_t)(r & mask_);
        const size_t first = n < cap_ - start ? n : cap_ - start;
        std::memcpy(dst, &buf_[start * ch_], first * ch_ * sizeof(float));
        if (n > first) std::memcpy(dst + first * ch_, &buf_[0], (n - first) * ch_ * sizeof(float));
    }
    size_t ch_ = kChannels, cap_ = 0, mask_ = 0;
    std::unique_ptr<float[]> buf_;
    alignas(64) std::atomic<uint64_t> write_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
};

} // namespace usbdirect
#endif

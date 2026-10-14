#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

#include "check.h"
#include "usbdirect/spsc_float_fifo.h"

using usbdirect::SpscFloatFifo;

static void frame(float *f, uint64_t n) {
    for (int c = 0; c < 4; c++) f[c] = (float)((n * 4 + (uint64_t)c) % 16777216ull); /* exact in float */
}

int main() {
    { /* runtime channel count (2-ch): wrap-around copy keeps frames intact */
        SpscFloatFifo f(8192, 2);
        CHECK(f.channels() == 2 && SpscFloatFifo(8192).channels() == 4);
        std::vector<float> in(6000 * 2), out(6000 * 2);
        for (size_t i = 0; i < in.size(); i++) in[i] = (float)i;
        for (int round = 0; round < 5; round++) {
            CHECK(f.push(in.data(), 6000) == 6000);
            CHECK(f.pop(out.data(), 6000) == 6000 && out == in);
        }
    }
    { /* runtime channel count (2-ch): wrap-around copy keeps frames intact */
        SpscFloatFifo f(8192, 2);
        CHECK(f.channels() == 2 && SpscFloatFifo(8192).channels() == 4);
        std::vector<float> in(6000 * 2), out(6000 * 2);
        for (size_t i = 0; i < in.size(); i++) in[i] = (float)i;
        for (int round = 0; round < 5; round++) {
            CHECK(f.push(in.data(), 6000) == 6000);
            CHECK(f.pop(out.data(), 6000) == 6000 && out == in);
        }
    }
    { /* capacity: power of two, at least 8192 */
        SpscFloatFifo a(1), b(8193), c(16384), d(8192);
        CHECK(a.capacity() == 8192 && b.capacity() == 16384 && c.capacity() == 16384 && d.capacity() == 8192);
    }
    { /* empty / full / partial accept */
        SpscFloatFifo f(8192);
        std::vector<float> in(9000 * 4), out(9000 * 4);
        for (size_t i = 0; i < 9000; i++) frame(&in[i * 4], i);
        float one[4];
        CHECK(f.size() == 0 && f.pop(one, 1) == 0 && f.writable() == 8192);
        CHECK(f.push(in.data(), 9000) == 8192); /* full: partial accept */
        CHECK(f.size() == 8192 && f.writable() == 0 && f.push(in.data(), 1) == 0);
        CHECK(f.pop(out.data(), 9000) == 8192 && f.size() == 0);
        for (size_t i = 0; i < 8192 * 4; i++) CHECK(out[i] == in[i]);
        CHECK(f.pop(out.data(), 1) == 0);
    }
    { /* wrap-around: odd-sized pushes/pops cross the end many times, order preserved */
        SpscFloatFifo f(8192);
        uint64_t wr = 0, rd = 0;
        std::vector<float> buf(4 * 5000);
        for (int iter = 0; iter < 200; iter++) {
            size_t n = 1 + (size_t)(iter * 997 % 5000);
            for (size_t i = 0; i < n; i++) frame(&buf[i * 4], wr + i);
            size_t pushed = f.push(buf.data(), n);
            wr += pushed;
            size_t m = 1 + (size_t)(iter * 613 % 5000);
            size_t got = f.pop(buf.data(), m);
            CHECK(got <= m);
            for (size_t i = 0; i < got; i++) {
                float e[4];
                frame(e, rd + i);
                for (int c = 0; c < 4; c++) CHECK(buf[i * 4 + c] == e[c]);
            }
            rd += got;
            CHECK(f.size() == wr - rd);
        }
        CHECK(f.totalPushed() == wr && f.totalPopped() == rd && wr > 8192);
    }
    { /* SPSC stress: two threads, 10M frames, no loss / duplication / reordering */
        SpscFloatFifo f(16384);
        const uint64_t total = 10000000ull;
        std::atomic<bool> bad{false};
        std::atomic<uint64_t> consumed{0};
        std::thread prod([&] {
            float buf[4 * 61];
            uint64_t n = 0;
            while (n < total) {
                size_t want = total - n < 61 ? (size_t)(total - n) : 61;
                for (size_t i = 0; i < want; i++) frame(&buf[i * 4], n + i);
                size_t done = 0;
                while (done < want) { /* retry the unaccepted tail */
                    size_t k = f.push(buf + done * 4, want - done);
                    done += k;
                    if (!k) std::this_thread::yield();
                }
                n += want;
            }
        });
        std::thread cons([&] {
            float buf[4 * 45];
            uint64_t n = 0;
            while (n < total) {
                size_t got = f.pop(buf, 45);
                if (!got) {
                    std::this_thread::yield();
                    continue;
                }
                for (size_t i = 0; i < got; i++) {
                    float e[4];
                    frame(e, n + i);
                    for (int c = 0; c < 4; c++)
                        if (buf[i * 4 + c] != e[c]) bad = true;
                }
                n += got;
            }
            consumed = n;
        });
        prod.join();
        cons.join();
        CHECK(!bad.load());
        CHECK(consumed.load() == total);
        CHECK(f.size() == 0 && f.totalPushed() == total && f.totalPopped() == total);
    }
    PASS("fifo");
}

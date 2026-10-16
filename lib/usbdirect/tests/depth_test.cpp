#include <cmath>

#include "check.h"
#include "usbdirect/depth_controller.h"

using usbdirect::DepthController;
using R = usbdirect::DepthController::Reason;
static const uint64_t MS = 1000000ull;

/* Feed `seconds` of 1 ms gaps with one `peak` gap per 100 ms, advancing t. */
static void run(DepthController &c, uint64_t &t, unsigned seconds, uint64_t peakNs) {
    for (unsigned i = 0; i < seconds * 1000; i++) {
        t += MS;
        c.onGap(i % 100 == 50 ? peakNs : MS, t);
    }
}

int main() {
    { /* start and clamp */
        CHECK(DepthController().depth() == 12);
        CHECK(DepthController(1).depth() == 4 && DepthController(99).depth() == 16 && DepthController(8).depth() == 8);
        DepthController c;
        CHECK(c.changeCount() == 0 && c.lastReason() == R::None);
    }
    { /* immediate raise: max(depth+2, ceil(gap_ms)+2), cap 16; gap == depth is not an underrun */
        DepthController c;
        CHECK(!c.onGap(12 * MS, 1) && c.depth() == 12); /* exactly 12 ms: not > depth */
        CHECK(c.onGap(12 * MS + 1, 2) && c.depth() == 15); /* ceil(12.000001)=13, +2 > depth+2 */
        CHECK(c.lastReason() == R::GapExceeded && c.changeCount() == 1);
        DepthController d(6);
        CHECK(d.onGap(6 * MS + 1, 1) && d.depth() == 9); /* ceil=7, +2 */
        DepthController d2(6);
        CHECK(d2.onGap(7 * MS, 1) && d2.depth() == 9); /* exactly 7: ceil 7, +2 */
        DepthController e(12);
        CHECK(e.onGap(13 * MS, 1) && e.depth() == 15);
        CHECK(e.onGap(40 * MS, 2) && e.depth() == 16); /* cap */
        CHECK(!e.onGap(40 * MS, 3) && e.depth() == 16 && e.changeCount() == 2); /* at cap: no change */
        DepthController f(4);
        CHECK(f.onGap(4 * MS + 1, 1) && f.depth() == 7);
        DepthController g(15);
        CHECK(g.onMissingSlot(1) && g.depth() == 16 && g.lastReason() == R::MissingSlot);
        CHECK(!g.onMissingSlot(2) && g.changeCount() == 1);
    }
    { /* windowed lower: nothing before 120 s; too few samples never lowers */
        DepthController c;
        uint64_t t = 0;
        run(c, t, 119, 3 * MS);
        CHECK(c.depth() == 12);
        run(c, t, 2, 3 * MS);
        CHECK(c.depth() == 11 && c.lastReason() == R::WindowLower && c.changeCount() == 1);
        DepthController sparse; /* 500 gaps over many windows: p99.99 unknown */
        for (uint64_t i = 1; i <= 500; i++) sparse.onGap(2 * MS, i * 1000 * MS);
        CHECK(sparse.depth() == 12 && sparse.changeCount() == 0);
    }
    { /* L1: start 6, min 2, 20 s window: one step per window down to the floor */
        DepthController c(6, 2, 16, 20000ull * MS);
        uint64_t t = 0;
        CHECK(c.depth() == 6 && c.minDepth() == 2);
        run(c, t, 19, 1 * MS);
        CHECK(c.depth() == 6);
        run(c, t, 2, 1 * MS);
        CHECK(c.depth() == 5 && c.lastReason() == R::WindowLower);
        for (int i = 0; i < 6; i++) run(c, t, 21, 1 * MS);
        CHECK(c.depth() == 2); /* target ceil(1+1)=2 = floor */
    }
    { /* never below target; target = ceil(max_ms + 1); at most one step per window */
        DepthController c(16);
        uint64_t t = 0;
        unsigned windows = 0;
        while (c.depth() > 8 && windows < 20) {
            run(c, t, 121, 6500000ull); /* max 6.5 ms -> target 8 */
            windows++;
        }
        CHECK(c.depth() == 8 && windows == 8); /* 16 -> 8, exactly one step per window */
        run(c, t, 400, 6500000ull);
        CHECK(c.depth() == 8); /* target reached: holds */
        CHECK(c.changeCount() == 8);
    }
    { /* a raise restarts the clean window */
        DepthController c;
        uint64_t t = 0;
        run(c, t, 100, 2 * MS);
        t += MS;
        CHECK(c.onGap(13 * MS, t) && c.depth() == 15);
        run(c, t, 100, 2 * MS); /* only ~100 s clean since the spike */
        CHECK(c.depth() == 15);
        run(c, t, 21, 2 * MS);
        CHECK(c.depth() == 14);
    }
    { /* floor clamp 4 */
        DepthController c(5);
        uint64_t t = 0;
        for (int i = 0; i < 6; i++) run(c, t, 121, MS);
        CHECK(c.depth() == 4);
    }
    { /* BLU-like trace A: the first excursion is the 14 ms gap -> 14+2 -> the 16 cap, and it holds */
        DepthController c;
        uint64_t t = 0;
        const uint64_t gaps[] = {8 * MS, 9 * MS, 14 * MS, 10 * MS, 13 * MS, 11 * MS, 12 * MS, 14 * MS};
        for (int pass = 0; pass < 3; pass++)
            for (uint64_t g : gaps) {
                t += MS;
                c.onGap(g, t);
            }
        CHECK(c.depth() == 16 && c.changeCount() == 1);
        for (int w = 0; w < 4; w++) /* minutes of continued <=14.2 ms gaps */
            for (unsigned i = 0; i < 121000; i++) {
                t += MS;
                c.onGap(i % 400 == 7 ? 14200000ull : MS, t);
            }
        CHECK(c.depth() == 16); /* target ceil(14.2 + 1) = 16: holds at the cap */
        CHECK(c.changeCount() == 1);
    }
    { /* BLU-like trace B: ascending 8..14 ms. 13 ms raises 12 -> 15 (13+2), 14 ms then fits; max 14.0
         gives target 15, so it settles at 15, not 16 (formula as specified). */
        DepthController c;
        uint64_t t = 0;
        for (int pass = 0; pass < 3; pass++)
            for (uint64_t g = 8; g <= 14; g++) {
                t += MS;
                c.onGap(g * MS, t);
            }
        CHECK(c.depth() == 15 && c.changeCount() == 1);
        for (int w = 0; w < 3; w++)
            for (unsigned i = 0; i < 121000; i++) {
                t += MS;
                c.onGap(i % 400 == 7 ? 14 * MS : MS, t);
            }
        CHECK(c.depth() == 15 && c.changeCount() == 1);
    }
    { /* Fold-like trace: gaps <= 4.6 ms never exceed 12, depth walks down and settles at 6 */
        DepthController c;
        uint64_t t = 0;
        for (int w = 0; w < 10; w++)
            for (unsigned i = 0; i < 121000; i++) {
                t += MS;
                c.onGap(i % 250 == 3 ? 4600000ull : MS, t);
            }
        CHECK(c.depth() == 6); /* ceil(4.6 + 1) = 6 */
        CHECK(c.changeCount() == 6 && c.lastReason() == R::WindowLower);
    }
    PASS("depth_controller");
}

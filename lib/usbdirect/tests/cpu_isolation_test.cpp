/* planIsolation arithmetic and sweepThreads on real threads. */
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <thread>

#include "check.h"
#include "usbdirect/cpu_isolation.h"

using namespace usbdirect;

static int selfTid() { return (int)syscall(SYS_gettid); }

int main() {
    { /* BLU: allowed 0-7, event cpu7, pump cpu6 */
        const IsolationPlan p = planIsolation(0xFF, 0x80, 0x40);
        CHECK(p.apply && p.audioMask == 0xC0 && p.otherMask == 0x3E);
    }
    { /* automatic: cpu0 left free (BLU 0xff, audio 0xc0 -> 0x3e) */
        const IsolationPlan p = planIsolation(0xFF, 0x80, 0x40);
        CHECK(p.apply && p.otherMask == 0x3E);
    }
    { /* override 0x3f restores cpu0; override is intersected with allowed & ~audio */
        const IsolationPlan p = planIsolation(0xFF, 0x80, 0x40, 0x3F);
        CHECK(p.apply && p.otherMask == 0x3F);
        const IsolationPlan q = planIsolation(0xFF, 0x80, 0x40, 0xFF);
        CHECK(q.apply && q.otherMask == 0x3F);
    }
    { /* override that empties out -> skip */
        CHECK(!planIsolation(0xFF, 0x80, 0x40, 0xC0).apply);
        CHECK(!planIsolation(0xFF, 0x80, 0x40, 0).apply);
    }
    { /* few CPUs: only 1 CPU would remain without cpu0 -> I3b value; 2 remain -> cpu0 dropped */
        const IsolationPlan a = planIsolation(0x0F, 0x08, 0x04); /* base 0x3 -> noCpu0 0x2 (1 cpu) */
        CHECK(a.apply && a.otherMask == 0x3);
        const IsolationPlan b = planIsolation(0x1F, 0x10, 0x08); /* base 0x7 -> 0x6 */
        CHECK(b.apply && b.otherMask == 0x6);
        const IsolationPlan c = planIsolation(0x07, 0x04, 0x02); /* base 0x1 only cpu0 */
        CHECK(c.apply && c.otherMask == 0x1);
    }
    { /* either mask 0 -> skip */
        CHECK(!planIsolation(0xFF, 0, 0x40).apply);
        CHECK(!planIsolation(0xFF, 0x80, 0).apply);
        CHECK(!planIsolation(0xFF, 0, 0).apply);
    }
    { /* audio CPUs cover the whole allowed set -> otherMask empty -> skip */
        CHECK(!planIsolation(0xC0, 0x80, 0x40).apply);
        CHECK(!planIsolation(0x80, 0x80, 0x80).apply);
    }
    { /* audio CPU outside the allowed set -> skip */
        CHECK(!planIsolation(0x3F, 0x80, 0x40).apply);
    }
    { /* overlapping event/pump masks are fine */
        const IsolationPlan p = planIsolation(0xFF, 0xC0, 0x40);
        CHECK(p.apply && p.audioMask == 0xC0 && p.otherMask == 0x3E);
    }
    { /* sweep: a helper thread is moved, a skipped one is not; a missing dir is harmless */
        uint64_t orig = 0;
        CHECK(threadMask(0, &orig));
        int cpu = -1;
        for (int i = 0; i < 64 && cpu < 0; i++)
            if (orig & ((uint64_t)1 << i)) cpu = i;
        CHECK(cpu >= 0);
        std::atomic<int> tidA{0}, tidB{0};
        std::atomic<bool> stop{false};
        auto body = [&](std::atomic<int> &t) {
            t.store(selfTid());
            while (!stop.load()) usleep(1000);
        };
        std::thread a([&] { body(tidA); }), b([&] { body(tidB); });
        while (!tidA.load() || !tidB.load()) usleep(1000);
        const SweepResult r = sweepThreads((uint64_t)1 << cpu, {tidB.load()});
        CHECK(r.moved >= 2 && r.failed == 0); /* main + a (b skipped) */
        uint64_t ma = 0, mb = 0;
        CHECK(threadMask(tidA.load(), &ma) && threadMask(tidB.load(), &mb));
        CHECK(ma == ((uint64_t)1 << cpu));
        CHECK(mb == orig);
        const SweepResult back = sweepThreads(orig, {});
        CHECK(back.failed == 0);
        stop.store(true);
        a.join();
        b.join();
        const SweepResult none = sweepThreads(orig, {}, "/nonexistent-dir");
        CHECK(none.moved == 0 && none.failed == 0);
        uint64_t now = 0;
        CHECK(threadMask(0, &now) && now == orig);
    }
    PASS("cpu_isolation");
}

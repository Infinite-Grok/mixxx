#ifndef USBDIRECT_CPU_ISOLATION_H
#define USBDIRECT_CPU_ISOLATION_H
/* Core isolation helpers (header-only): keep every other thread of the process off the CPUs the USB event
 * thread and the engine pump run on.
 *
 * planIsolation(): pure mask arithmetic. apply=false (= do nothing) when either audio mask is 0, when the audio
 * CPUs do not lie inside the allowed set, or when no CPU would be left for the other threads.
 * sweepThreads(): sched_setaffinity() on every task of the process except the skipped tids. Failures are counted,
 * never fatal. processMask() reads sched_getaffinity(0). maskToSet()/setToMask() convert for CPUs 0..63. */
#include <dirent.h>
#include <sched.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace usbdirect {

struct IsolationPlan {
    bool apply = false;
    uint64_t audioMask = 0; /* event | pump */
    uint64_t otherMask = 0; /* allowed & ~audio */
};

/* overrideMask: -1 = automatic (allowed & ~audio & ~cpu0 when >= 2 CPUs remain, else allowed & ~audio; cpu0 is where
 * Android routes most device IRQs, incl. the xHCI); otherwise a mask used as override & allowed & ~audio (empty ->
 * skip). */
inline IsolationPlan planIsolation(uint64_t allowedMask, uint64_t eventMask, uint64_t pumpMask,
                                   int64_t overrideMask = -1) {
    IsolationPlan p;
    if (eventMask == 0 || pumpMask == 0) return p;
    p.audioMask = eventMask | pumpMask;
    if ((p.audioMask & ~allowedMask) != 0) return p; /* audio CPU not allowed: placement failed, do not isolate */
    const uint64_t base = allowedMask & ~p.audioMask;
    if (overrideMask >= 0) {
        p.otherMask = base & (uint64_t)overrideMask;
    } else {
        const uint64_t noCpu0 = base & ~(uint64_t)1;
        p.otherMask = (__builtin_popcountll(noCpu0) >= 2) ? noCpu0 : base;
    }
    if (p.otherMask == 0) return p;
    p.apply = true;
    return p;
}

inline bool setToMask(const cpu_set_t &set, uint64_t *out) {
    uint64_t m = 0;
    for (int i = 0; i < 64 && i < CPU_SETSIZE; i++)
        if (CPU_ISSET(i, &set)) m |= (uint64_t)1 << i;
    *out = m;
    return m != 0;
}

/* Affinity of the calling thread (tid 0) or of tid; false on error. */
inline bool threadMask(int tid, uint64_t *out) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity((pid_t)tid, sizeof(set), &set) != 0) return false;
    return setToMask(set, out);
}

inline bool setThreadMask(int tid, uint64_t mask) {
    if (mask == 0) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < 64 && i < CPU_SETSIZE; i++)
        if (mask & ((uint64_t)1 << i)) CPU_SET(i, &set);
    return sched_setaffinity((pid_t)tid, sizeof(set), &set) == 0;
}

struct SweepResult {
    unsigned moved = 0;
    unsigned failed = 0;
};

inline SweepResult sweepThreads(uint64_t mask, const std::vector<int> &skipTids,
                                const std::string &taskDir = "/proc/self/task") {
    SweepResult r;
    DIR *d = opendir(taskDir.c_str());
    if (!d) return r;
    while (struct dirent *e = readdir(d)) {
        char *end = nullptr;
        const long tid = std::strtol(e->d_name, &end, 10);
        if (end == e->d_name || *end != '\0' || tid <= 0) continue;
        bool skip = false;
        for (int s : skipTids)
            if (s == (int)tid) skip = true;
        if (skip) continue;
        if (setThreadMask((int)tid, mask))
            r.moved++;
        else
            r.failed++;
    }
    closedir(d);
    return r;
}

} // namespace usbdirect
#endif

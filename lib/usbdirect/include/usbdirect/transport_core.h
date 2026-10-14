/* Pure header-only port of tools/android-usb-probe/native/stream_core.h (G5 4f9f03e).
 * Semantics and limits are identical (parity-tested). No I/O, no allocation. */
#ifndef USBDIRECT_TRANSPORT_CORE_H
#define USBDIRECT_TRANSPORT_CORE_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <math.h>

#define T7_DENOM 163840u
#define T7_NOMINAL 7225344u /* (441/10) * 163840, exact */
#define T7_STARTUP_NS 250000000ull
#define T7_STALE_NS 128000000ull
#define T7_DRAIN_NS 2000000000ull

typedef struct {
    uint32_t phase, rate, valid;
    uint64_t first_ns, last_valid_ns;
} T7Clock;
/* 0 = zero packet, 1 = valid packet, -1 = malformed/implausible. */
inline int t7_feedback(T7Clock *c, const unsigned char *b, unsigned n, uint64_t now) {
    if (!n) return 0;
    if (n != 3) return -1;
    uint32_t q = b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16);
    /* Compare integers to exact 44.1 +/- 1%; no floating conversion/shift guessing. */
    if ((uint64_t)q * 1000 < 441ull * 16384 * 99 ||
        (uint64_t)q * 1000 > 441ull * 16384 * 101) return -1;
    c->rate = q * 10; c->valid++; c->last_valid_ns = now;
    return 1;
}
inline int t7_clock_error(const T7Clock *c, uint64_t now) {
    if (c->valid < 3 && now - c->first_ns >= T7_STARTUP_NS) return 1;
    if (c->valid && now - c->last_valid_ns >= T7_STALE_NS) return 2;
    return 0;
}
inline int t7_frames(T7Clock *c, unsigned capacity) {
    uint64_t total = (uint64_t)c->phase + c->rate;
    unsigned frames = (unsigned)(total / T7_DENOM);
    if (frames * 12 > capacity) return -1;
    c->phase = (uint32_t)(total % T7_DENOM);
    return (int)frames;
}
inline void t7_pack24(unsigned char *b, double x) {
    if (x > 1) x = 1;
    if (x < -1) x = -1;
    int32_t s = (int32_t)llround(x * (x<0 ? 8388608.0 : 8388607.0));
    b[0] = (unsigned char)s; b[1] = (unsigned char)(s >> 8); b[2] = (unsigned char)(s >> 16);
}
inline void t7_pcm(unsigned char *b, unsigned frames, uint64_t *sample, unsigned channels, int tone) {
    static const double freq[4] = {440,660,880,1100};
    for (unsigned i=0; i<frames; i++, (*sample)++)
        for (unsigned ch=0; ch<4; ch++)
            t7_pack24(b + i*12 + ch*3, tone && (channels & (1u<<ch)) ?
                0.02 * sin(6.283185307179586 * freq[ch] * (double)*sample / 44100.0) : 0);
}
/* Shared submission ownership accounting: callback must reap before any resubmit. */
typedef struct { unsigned live; int stopping; uint64_t stop_ns; } T7Life;
inline int t7_may_submit(const T7Life *l) { return !l->stopping; }
inline void t7_submitted(T7Life *l) { l->live++; }
inline void t7_reaped(T7Life *l) { if(l->live) l->live--; }
inline void t7_stop(T7Life *l, uint64_t now) { if(!l->stopping) {l->stopping=1;l->stop_ns=now;} }
inline int t7_pending(const T7Life *l, uint64_t now) {
    return l->stopping && l->live && now-l->stop_ns >= T7_DRAIN_NS;
}
/* G3 underrun evidence: fixed-size aggregate OUT completion gap counters (no allocation). */
#define T7_CAND_MAX 64u
typedef struct {
    uint64_t total, exceed;      /* all gaps; gaps strictly greater than depth ms */
    uint64_t hist[6];            /* strictly greater than 1.5, 2, 3, 4, 5, 8 ms */
    unsigned ncand;
    uint64_t cand_t[T7_CAND_MAX], cand_gap[T7_CAND_MAX]; /* first candidates only */
} T7Gaps;
inline void t7_gap_record(T7Gaps *g, unsigned depth, uint64_t gap, uint64_t t_from_start) {
    static const uint64_t edge[6] = {1500000ull, 2000000ull, 3000000ull, 4000000ull, 5000000ull, 8000000ull};
    g->total++;
    for (unsigned i=0; i<6; i++) if (gap > edge[i]) g->hist[i]++;
    if (gap > (uint64_t)depth * 1000000ull) {
        g->exceed++;
        if (g->ncand < T7_CAND_MAX) {g->cand_t[g->ncand]=t_from_start; g->cand_gap[g->ncand]=gap; g->ncand++;}
    }
}
/* Whole 1 ms packets elapsed from first OUT submission (0 = never) to stop. */
inline uint64_t t7_expected_packets(uint64_t first_out_ns, uint64_t stop_ns) {
    return (first_out_ns && stop_ns > first_out_ns) ? (stop_ns - first_out_ns) / 1000000ull : 0;
}
/* G4 optional CPU affinity helpers (pure; OS calls live in stream.c). */
#define T7_CPU_MAX 16u
typedef struct {uint64_t n[T7_CPU_MAX]; uint64_t other;} T7CpuSamples; /* other = cpu <0 or >=16 */
inline int t7_affinity_valid(long long mask) {return mask>=0 && mask<=255;}
inline int t7_mask_has_cpu(uint64_t mask, int cpu) {return cpu>=0 && cpu<64 && ((mask>>cpu)&1u);}
inline void t7_hex_mask(char *out, size_t cap, uint64_t mask) {snprintf(out,cap,"0x%llx",(unsigned long long)mask);}
inline void t7_cpu_sample(T7CpuSamples *c, int cpu) {
    if (cpu>=0 && (unsigned)cpu<T7_CPU_MAX) c->n[cpu]++; else c->other++;
}
/* G5 optional ADPF hint helpers (pure; platform calls live in stream.c under __ANDROID__). */
#define T7_ADPF_MIN_NS 1000ull
#define T7_ADPF_MAX_NS 1000000ull
enum {T7_ADPF_UNAVAILABLE=0, T7_ADPF_OK=1, T7_ADPF_FAILED=2};
typedef struct {uint64_t last_ns; int primed;} T7AdpfLimiter;
inline uint64_t t7_adpf_clamp(uint64_t ns) {return ns<T7_ADPF_MIN_NS?T7_ADPF_MIN_NS:ns>T7_ADPF_MAX_NS?T7_ADPF_MAX_NS:ns;}
/* True at most once per millisecond; first call is always due; a non-advancing clock is never due. */
inline int t7_adpf_due(T7AdpfLimiter *l, uint64_t now_ns) {
    if (l->primed && (now_ns<l->last_ns || now_ns-l->last_ns<T7_ADPF_MAX_NS)) return 0;
    l->primed=1;l->last_ns=now_ns;return 1;
}
inline int t7_adpf_api_ok(int api) {return api>=33;}
inline const char *t7_adpf_session_name(int s) {return s==T7_ADPF_OK?"ok":s==T7_ADPF_FAILED?"failed":"unavailable";}
#endif

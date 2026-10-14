/* UsbAudioOut against a fake libusb (tests/fake_libusb.cpp), driven in manual mode. */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

#include "check.h"
#include "fake_libusb.h"
#include "usbdirect/depth_controller.h"
#include "usbdirect/device_profile.h"
#include "usbdirect/transport_core.h"
#include "usbdirect/usb_audio_out.h"

using namespace usbdirect;

/* ---- allocation tracking: nothing may allocate inside a libusb completion callback ---- */
static long g_cb_allocs = 0;
__attribute__((noinline)) void *operator new(size_t n) {
    if (fake::in_callback && !fake::in_fake) g_cb_allocs++;
    void *p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
__attribute__((noinline)) void operator delete(void *p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void *p, size_t) noexcept { std::free(p); }

static const uint64_t MS = 1000000ull;
static uint64_t g_now = 1000 * MS;
static uint64_t fakeClock() { return g_now; }

/* Deterministic source: frame n, channel c. */
static float sample(uint64_t n, int c) { return (float)((n * 7 + (uint64_t)c * 13) % 20000) / 40000.0f - 0.25f; }

struct Rig {
    Options opt;
    UsbAudioOut *dev = nullptr;
    unsigned char raw[kT7DescriptorLen];
    uint64_t pushed = 0;     /* frames pushed into the FIFO */
    uint64_t verified = 0;   /* frames verified in submitted OUT packets */
    uint64_t mismatches = 0, badOutLive = 0, ticks = 0, shortfallMode = 0;
    unsigned minOutLive = 99, maxOutLive = 0;
    T7Clock mirror{}; /* independent replay of the packet-size clock */

    explicit Rig(unsigned depth = 12) {
        fake::reset();
        g_now = 1000 * MS;
        opt.spawnThread = false;
        opt.setPriority = false;
        opt.clock = fakeClock;
        opt.startDepth = depth;
        std::memcpy(raw, kT7Descriptor, sizeof(raw));
        mirror.rate = T7_NOMINAL;
        dev = new UsbAudioOut(opt);
    }
    ~Rig() { delete dev; }

    void feed(size_t frames) {
        std::vector<float> buf(frames * 4);
        for (size_t i = 0; i < frames; i++)
            for (int c = 0; c < 4; c++) buf[i * 4 + (size_t)c] = sample(pushed + i, c);
        pushed += dev->fifo().push(buf.data(), frames);
    }
    /* Check newly submitted OUT packets against the reference stream, in order. */
    void verifySubs() {
        for (const auto &s : fake::subs) {
            if (s.ep != 0x01) continue;
            CHECK(s.length % 12 == 0 && s.length >= 0 && s.length <= 540);
            CHECK(t7_frames(&mirror, 540) * 12 == s.length); /* packet sizes follow the clock with no skipped packet */
            for (int f = 0; f < s.length / 12; f++, verified++) {
                for (int c = 0; c < 4; c++) {
                    unsigned char want[3];
                    t7_pack24(want, verified < pushed ? (double)sample(verified, c) : 0.0);
                    if (std::memcmp(s.data + f * 12 + c * 3, want, 3)) mismatches++;
                }
            }
        }
        fake::subs.clear();
    }
    libusb_transfer *oldest(unsigned char ep) {
        for (auto *t : fake::pending)
            if (t->endpoint == ep) return t;
        return nullptr;
    }
    /* One device millisecond: complete the oldest OUT; feedback early and every 32 ms. */
    void tick(uint64_t gap = MS, bool feedFifo = true) {
        g_now += gap;
        ticks++;
        if (feedFifo) feed(45);
        if (libusb_transfer *fb = oldest(0x81)) {
            if (ticks < 5 || ticks % 32 == 0) {
                fb->buffer[0] = 0x66;
                fb->buffer[1] = 0x06;
                fb->buffer[2] = 0x0b;
                t7_feedback(&mirror, fb->buffer, 3, g_now);
                fake::complete(fb, LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_COMPLETED, 3);
            }
        }
        if (libusb_transfer *out = oldest(0x01))
            fake::complete(out, LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_COMPLETED, out->length);
        unsigned live = (unsigned)fake::pending_on(0x01);
        if (live < minOutLive) minOutLive = live;
        if (live > maxOutLive) maxOutLive = live;
        verifySubs();
    }
    void open() { CHECK(dev->open(7, raw, sizeof(raw)) == OpenResult::Ok); }
    void startRunning(size_t prefill = 6000) {
        open();
        feed(prefill);
        CHECK(dev->start());
        verifySubs();
    }
};

static void testGate() {
    Rig r;
    FILE *f = std::fopen("../../docs/verification/evidence/android-usb-t7-20261012/raw-descriptors.bin", "rb");
    CHECK(f != nullptr);
    unsigned char cap[kT7DescriptorLen + 1];
    size_t n = std::fread(cap, 1, sizeof(cap), f);
    std::fclose(f);
    CHECK(n == kT7DescriptorLen && !std::memcmp(cap, kT7Descriptor, n)); /* allowlist == real G1 capture */
    CHECK(kT7DescriptorLen == 288);
    unsigned char bad[kT7DescriptorLen];
    std::memcpy(bad, kT7Descriptor, sizeof(bad));
    bad[1] ^= 1;
    CHECK(r.dev->open(7, bad, sizeof(bad)) == OpenResult::UnsupportedDevice);
    bad[1] ^= 1;
    bad[sizeof(bad) - 1] ^= 0x80;
    CHECK(r.dev->open(7, bad, sizeof(bad)) == OpenResult::UnsupportedDevice);
    CHECK(r.dev->open(7, kT7Descriptor, kT7DescriptorLen - 1) == OpenResult::UnsupportedDevice);
    CHECK(r.dev->open(7, kT7Descriptor, kT7DescriptorLen + 1) == OpenResult::UnsupportedDevice);
    CHECK(r.dev->open(7, nullptr, kT7DescriptorLen) == OpenResult::BadArgs);
    CHECK(r.dev->open(-1, kT7Descriptor, kT7DescriptorLen) == OpenResult::BadArgs);
    CHECK(fake::calls.empty()); /* nothing touched USB before the gate passed */
    CHECK(r.dev->open(7, r.raw, sizeof(r.raw)) == OpenResult::Ok);
    const std::vector<std::string> expect = {"init", "wrap", "speed", "driver0", "driver1", "driver2",
                                             "driver3", "claim1", "alt1=1"};
    CHECK(fake::calls == expect);
    CHECK(fake::allocs == 18);
    CHECK(r.dev->open(7, r.raw, sizeof(r.raw)) == OpenResult::AlreadyOpen);
    r.dev->stop();
    CHECK(!r.dev->detachedKernelDriver() && !fake::has("attach1") && !fake::has("detach1"));
}

static void testOpenFailures() {
    { /* BUSY + active driver: one detach, one retry; reattach at stop */
        Rig r;
        fake::cfg.driver[1] = 1;
        fake::cfg.claim_rc[0] = LIBUSB_ERROR_BUSY;
        r.open();
        CHECK(fake::index_of("detach1") > fake::index_of("claim1") && fake::index_of("claim1", fake::index_of("detach1")) > 0);
        CHECK(r.dev->detachedKernelDriver());
        r.dev->stop();
        CHECK(fake::has("attach1") && fake::index_of("attach1") > fake::index_of("release1"));
    }
    { /* BUSY without active driver: no detach; fail and release context */
        Rig r;
        fake::cfg.claim_rc[0] = LIBUSB_ERROR_BUSY;
        CHECK(r.dev->open(7, r.raw, sizeof(r.raw)) == OpenResult::ClaimFailed);
        CHECK(!fake::has("detach1") && fake::has("close") && fake::has("exit") && !fake::has("release1"));
    }
    { /* retry also busy: no second detach, reattach what we detached */
        Rig r;
        fake::cfg.driver[1] = 1;
        fake::cfg.claim_rc[0] = fake::cfg.claim_rc[1] = LIBUSB_ERROR_BUSY;
        CHECK(r.dev->open(7, r.raw, sizeof(r.raw)) == OpenResult::ClaimFailed);
        int detaches = 0;
        for (auto &c : fake::calls) detaches += c == "detach1";
        CHECK(detaches == 1 && fake::has("attach1") && !fake::has("release1"));
    }
    { /* other claim error never detaches */
        Rig r;
        fake::cfg.driver[1] = 1;
        fake::cfg.claim_rc[0] = LIBUSB_ERROR_ACCESS;
        CHECK(r.dev->open(7, r.raw, sizeof(r.raw)) == OpenResult::ClaimFailed && !fake::has("detach1"));
    }
    { /* each failing stage maps to its result and still closes everything */
        Rig a;
        fake::cfg.init_rc = LIBUSB_ERROR_OTHER;
        CHECK(a.dev->open(7, a.raw, sizeof(a.raw)) == OpenResult::InitFailed);
        Rig b;
        fake::cfg.wrap_rc = LIBUSB_ERROR_ACCESS;
        CHECK(b.dev->open(7, b.raw, sizeof(b.raw)) == OpenResult::WrapFailed && fake::has("exit") && !fake::has("close"));
        Rig c;
        fake::cfg.speed = LIBUSB_SPEED_HIGH;
        CHECK(c.dev->open(7, c.raw, sizeof(c.raw)) == OpenResult::SpeedRejected && !fake::has("claim1") && fake::has("close"));
        Rig d;
        fake::cfg.alt_rc = LIBUSB_ERROR_IO;
        CHECK(d.dev->open(7, d.raw, sizeof(d.raw)) == OpenResult::Alt1Failed);
        CHECK(fake::has("alt1=0") && fake::has("release1") && fake::has("close") && fake::has("exit"));
    }
}

static void testExactStream() {
    Rig r;
    r.startRunning();
    CHECK(fake::pending_on(0x81) == 2 && fake::pending_on(0x01) == 12);
    Stats s0 = r.dev->stats();
    CHECK(s0.state == State::Running && s0.depth == 12 && s0.packetsSubmitted == 12 && s0.outLive == 12);
    for (int i = 0; i < 3000; i++) r.tick();
    CHECK(r.mismatches == 0);
    CHECK(r.minOutLive == 12 && r.maxOutLive == 12); /* replaced in the same completion */
    CHECK(fake::pending_on(0x01) == 12);
    Stats s = r.dev->stats();
    CHECK(s.packetsSubmitted == 12 + 3000 && s.outCompleted == 3000 && s.packetsCompletedOk == 3000);
    CHECK(s.depth == 12 && s.depthChanges == 0 && s.engineUnderruns == 0 && s.errors == 0);
    CHECK(s.gapTotal == 2999 && s.gapExceedDepth == 0 && s.candidates == 0 && s.missingSlotEvents == 0);
    CHECK(s.feedbackValid >= 3 && s.feedbackInvalid == 0);
    CHECK(s.feedbackRate == 722534 * 10 && s.feedbackHz > 44099.9 && s.feedbackHz < 44100.0);
    CHECK(s.fifoCapacity == 16384 && s.fifoFill == r.pushed - r.dev->fifo().totalPopped());
    CHECK(s.framesSubmitted == r.dev->fifo().totalPopped() && r.verified == s.framesSubmitted);
    /* 3012 packets at 44.0 frames average (feedback 44.1 minus 1 LSB) */
    CHECK(s.framesSubmitted > 3012ull * 44 && s.framesSubmitted < 3012ull * 45);
    r.dev->stop();
    CHECK(g_cb_allocs == 0);
}

static void testShortfall() {
    Rig r;
    r.open();
    r.feed(100); /* far less than 12 packets of 44 frames */
    CHECK(r.dev->start());
    r.verifySubs();
    Stats s = r.dev->stats();
    CHECK(s.engineUnderruns >= 1 && s.engineUnderrunFrames == s.framesSubmitted - 100);
    CHECK(r.dev->fifo().size() == 0 && r.mismatches == 0); /* 100 real frames then zeros */
    for (int i = 0; i < 400; i++) r.tick(MS, false);
    s = r.dev->stats();
    CHECK(r.mismatches == 0 && r.verified == s.framesSubmitted);
    CHECK(s.engineUnderruns == s.packetsSubmitted - 2); /* all but the packets served from 100 frames */
    CHECK(s.engineUnderrunFrames == s.framesSubmitted - 100);
    /* engine underruns are distinct from USB gap candidates */
    CHECK(s.gapExceedDepth == 0 && s.candidates == 0 && s.depth == 12 && s.depthChanges == 0);
    r.dev->stop();
}

static void testDepthRaise() {
    Rig r;
    r.startRunning();
    for (int i = 0; i < 500; i++) r.tick();
    CHECK(fake::pending_on(0x01) == 12);
    r.tick(14500000ull); /* one 14.5 ms completion gap > 12 ms */
    Stats s = r.dev->stats();
    CHECK(s.depth == 16 && s.depthChanges == 1 && s.depthReason == (int)DepthController::Reason::GapExceeded);
    CHECK(s.gapExceedDepth == 1 && s.candidates == 1 && s.candidateGap[0] == 14500000ull);
    CHECK(fake::pending_on(0x01) == 16 && s.outLive == 16); /* raised inside that very completion */
    for (int i = 0; i < 1000; i++) r.tick();
    CHECK(fake::pending_on(0x01) == 16);
    CHECK(r.mismatches == 0); /* the stream across the raise is contiguous: no gap, no duplicate */
    CHECK(r.verified == r.dev->stats().framesSubmitted);
    CHECK(s.engineUnderruns == 0 && r.dev->stats().engineUnderruns == 0);
    r.dev->stop();
    CHECK(g_cb_allocs == 0);
}

static void testDepthLower() {
    Rig r;
    r.startRunning();
    uint64_t guard = 0;
    for (int i = 0; i < 119000; i++) r.tick();
    CHECK(r.dev->stats().depth == 12 && fake::pending_on(0x01) == 12);
    for (int i = 0; i < 2500 && r.dev->stats().depth == 12; i++, guard++) r.tick();
    Stats s = r.dev->stats();
    CHECK(s.depth == 11 && s.depthChanges == 1 && s.depthReason == (int)DepthController::Reason::WindowLower);
    CHECK(fake::pending_on(0x01) == 11 && s.outLive == 11); /* slot retired in that completion, no resubmit */
    for (int i = 0; i < 1000; i++) r.tick();
    CHECK(fake::pending_on(0x01) == 11);
    CHECK(r.mismatches == 0 && r.verified == r.dev->stats().framesSubmitted); /* continuous across the lower */
    for (int i = 0; i < 121000; i++) r.tick(); /* second clean window -> 10, one step per window */
    s = r.dev->stats();
    CHECK(s.depth == 10 && s.depthChanges == 2 && fake::pending_on(0x01) == 10);
    CHECK(r.mismatches == 0 && r.verified == s.framesSubmitted && s.engineUnderruns == 0);
    CHECK(s.gapExceedDepth == 0);
    /* a later excursion raises again from the lowered depth (10 -> 13) without gaps */
    r.tick(11 * MS);
    s = r.dev->stats();
    CHECK(s.depth == 13 && fake::pending_on(0x01) == 13);
    for (int i = 0; i < 200; i++) r.tick();
    CHECK(r.mismatches == 0 && r.verified == r.dev->stats().framesSubmitted);
    r.dev->stop();
    CHECK(g_cb_allocs == 0);
}

static void expectCleanupOrder(bool detached) {
    const auto &c = fake::calls;
    int lastCancel = -1, firstFree = -1;
    for (int i = 0; i < (int)c.size(); i++) {
        if (c[i] == "cancel") lastCancel = i;
        if (c[i] == "free" && firstFree < 0) firstFree = i;
    }
    CHECK(lastCancel >= 0 && firstFree > lastCancel);
    int i = fake::index_of("alt1=0");
    CHECK(i > firstFree);
    std::vector<std::string> tail(c.begin() + i, c.end());
    std::vector<std::string> want = {"alt1=0", "release1"};
    if (detached) want.push_back("attach1");
    for (const char *s : {"driver0", "driver1", "driver2", "driver3", "close", "exit"}) want.push_back(s);
    CHECK(tail == want);
}

static void testStopDrains() {
    {
        Rig r;
        r.startRunning();
        for (int i = 0; i < 100; i++) r.tick();
        CHECK(fake::pending.size() == 14);
        r.dev->stop();
        CHECK(fake::pending.empty() && fake::allocs == 18 && fake::frees == 18);
        Stats s = r.dev->stats();
        CHECK(s.state == State::Stopped && s.totalLive == 0 && s.outLive == 0 && s.reason == Halt::UserStop);
        expectCleanupOrder(false);
        CHECK(fake::event_calls >= 1);
        int rc = 0;
        for (int i = 0; i < r.dev->ownershipCount(); i++) rc += r.dev->ownershipTag(i) == 21 && r.dev->ownershipReturn(i) != 0;
        CHECK(rc == 0);
        r.dev->stop(); /* idempotent */
        CHECK(fake::frees == 18);
    }
    { /* with a detached driver the reattach comes after release and before close */
        Rig r;
        fake::cfg.driver[1] = 1;
        fake::cfg.claim_rc[0] = LIBUSB_ERROR_BUSY;
        r.startRunning();
        for (int i = 0; i < 10; i++) r.tick();
        r.dev->stop();
        expectCleanupOrder(true);
    }
    { /* destructor stops a running device */
        Rig r;
        r.startRunning();
        delete r.dev;
        r.dev = nullptr;
        CHECK(fake::pending.empty() && fake::frees == 18 && fake::has("exit"));
    }
    { /* open but never started: stop releases */
        Rig r;
        r.open();
        r.dev->stop();
        CHECK(fake::frees == 18 && fake::index_of("alt1=0") > 0);
    }
    { /* drain watchdog: events stall > 2 s; nothing freed until reaped; flag recorded */
        Rig r;
        r.startRunning();
        for (int i = 0; i < 10; i++) r.tick();
        fake::cfg.hold_events = 3;
        fake::events_hook = [] { g_now += 1000 * MS; };
        r.dev->stop();
        CHECK(r.dev->stats().drainWatchdog && fake::pending.empty() && fake::frees == 18);
        CHECK(fake::index_of("free") > fake::index_of("cancel"));
    }
}

static void testErrors() {
    { /* NO_DEVICE completion: stop streaming, never resubmit, drain the rest */
        Rig r;
        r.startRunning();
        for (int i = 0; i < 5; i++) r.tick();
        const int subs = fake::submits;
        fake::complete(r.oldest(0x01), LIBUSB_TRANSFER_NO_DEVICE, LIBUSB_TRANSFER_NO_DEVICE, 0);
        CHECK(r.dev->stats().reason == Halt::NoDevice && fake::submits == subs);
        r.dev->stop();
        CHECK(fake::pending.empty() && fake::submits == subs && fake::frees == 18);
    }
    { /* packet error */
        Rig r;
        r.startRunning();
        auto *t = r.oldest(0x01);
        fake::complete(t, LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_ERROR, t->length);
        CHECK(r.dev->stats().reason == Halt::TransferOrPacketError && r.dev->stats().errors == 1);
        r.dev->stop();
    }
    { /* strict actual length: zero is not success */
        Rig r;
        r.startRunning();
        fake::complete(r.oldest(0x01), LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_COMPLETED, 0);
        CHECK(r.dev->stats().reason == Halt::OutActualLengthMismatch);
        r.dev->stop();
    }
    { /* invalid (non-empty wrong size) feedback aborts; zero-length feedback does not */
        Rig r;
        r.startRunning();
        fake::complete(r.oldest(0x81), LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_COMPLETED, 0);
        CHECK(r.dev->stats().reason == Halt::None && r.dev->stats().feedbackZero == 1);
        auto *fb = r.oldest(0x81);
        fake::complete(fb, LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_COMPLETED, 2);
        CHECK(r.dev->stats().reason == Halt::InvalidFeedback && r.dev->stats().feedbackInvalid == 1);
        r.dev->stop();
    }
    { /* implausible Q14 value */
        Rig r;
        r.startRunning();
        auto *fb = r.oldest(0x81);
        fb->buffer[0] = 0;
        fb->buffer[1] = 0;
        fb->buffer[2] = 0x0c;
        fake::complete(fb, LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_COMPLETED, 3);
        CHECK(r.dev->stats().reason == Halt::InvalidFeedback);
        r.dev->stop();
    }
    { /* startup timeout: fewer than 3 valid packets within 250 ms */
        Rig r;
        r.startRunning();
        g_now += 251 * MS;
        fake::complete(r.oldest(0x01), LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_COMPLETED, r.oldest(0x01)->length);
        CHECK(r.dev->stats().reason == Halt::FeedbackStartupTimeout);
        r.dev->stop();
    }
    { /* stale feedback: 128 ms without a valid packet */
        Rig r;
        r.startRunning();
        for (int i = 0; i < 40; i++) r.tick();
        CHECK(r.dev->stats().feedbackValid >= 3 && r.dev->stats().reason == Halt::None);
        g_now += 129 * MS;
        auto *out = r.oldest(0x01);
        fake::complete(out, LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_COMPLETED, out->length);
        CHECK(r.dev->stats().reason == Halt::FeedbackStale);
        r.dev->stop();
    }
    { /* submit failure during start: reports false, first cause kept, everything reaped */
        Rig r;
        r.open();
        r.feed(6000);
        fake::cfg.fail_submit_at = 5;
        CHECK(!r.dev->start());
        CHECK(r.dev->stats().reason == Halt::NoDevice);
        r.dev->stop();
        CHECK(fake::pending.empty() && fake::frees == 18);
    }
    { /* event loop stop request halts via eventStep */
        Rig r;
        r.startRunning();
        r.dev->eventStep();
        CHECK(r.dev->stats().reason == Halt::None);
        r.dev->stop();
    }
}

static int g_lw = 0, g_lwBad = 0;
static void onLow(void *) {
    g_lw++;
    if (fake::in_callback) g_lwBad++;
}
static void testLowWater() {
    Rig r;
    g_lw = g_lwBad = 0;
    r.dev->setLowWaterCallback(onLow, nullptr, 1000);
    r.startRunning(8000);
    for (int i = 0; i < 50; i++) r.tick(MS, false); /* drains FIFO below 8000 - 2200, stays above 1000 */
    CHECK(g_lw == 0);
    r.dev->eventStep();
    CHECK(g_lw == 0);
    for (int i = 0; i < 150; i++) r.tick(MS, false); /* fill falls below 1000 inside completions */
    CHECK(g_lw == 0); /* never called from a libusb callback */
    r.dev->eventStep();
    CHECK(g_lw == 1 && g_lwBad == 0);
    r.feed(5000);
    r.dev->eventStep();
    CHECK(g_lw == 1);
    r.dev->stop();
}

/* Real event thread (host): concurrent producer, stats readers, and consistent snapshots. */
static void testThreaded() {
    Rig r;
    r.opt.spawnThread = true;
    delete r.dev;
    r.dev = new UsbAudioOut(r.opt);
    r.open();
    r.feed(12000);
    static uint64_t tickNo;
    tickNo = 0;
    static Rig *rig;
    rig = &r;
    fake::events_hook = [] { /* runs on the event thread inside handle_events */
        g_now += MS;
        tickNo++;
        if (auto *fb = rig->oldest(0x81)) {
            if (tickNo < 5 || tickNo % 32 == 0) {
                fb->buffer[0] = 0x66;
                fb->buffer[1] = 0x06;
                fb->buffer[2] = 0x0b;
                fake::complete(fb, LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_COMPLETED, 3);
            }
        }
        if (auto *out = rig->oldest(0x01)) fake::complete(out, LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_COMPLETED, out->length);
        fake::subs.clear();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    };
    CHECK(r.dev->start());
    uint64_t torn = 0, last = 0;
    std::vector<float> buf(2000 * 4, 0.1f);
    for (int i = 0; i < 400; i++) {
        r.dev->fifo().push(buf.data(), 2000);
        Stats s = r.dev->stats();
        if (s.packetsSubmitted != s.outCompleted + s.outLive) torn++; /* every published block is coherent */
        if (s.outCompleted < last) torn++;
        last = s.outCompleted;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    CHECK(torn == 0 && last > 50);
    r.dev->stop();
    Stats s = r.dev->stats();
    CHECK(s.state == State::Stopped && s.totalLive == 0 && fake::pending.empty() && fake::frees == 18);
    CHECK(s.framesSubmitted == r.dev->fifo().totalPopped() + s.engineUnderrunFrames);
}

/* I2a: format(), unknown-device rejection, usbQueuedFrames, low-water contract (I1 review 1 and 2). */
static UsbAudioOut *g_stopDev = nullptr;
static int g_stopCalls = 0;
static void stopFromLowWater(void *) {
    g_stopCalls++;
    g_stopDev->stop(); /* must only request, never join/clean up inside the callback */
}
static void testI2() {
    { /* format() and UnsupportedDevice (no USB call, no claim) */
        Rig r;
        CHECK(!r.dev->format().valid());
        unsigned char bad[kT7DescriptorLen];
        std::memcpy(bad, kT7Descriptor, sizeof(bad));
        bad[10] ^= 0x01; /* PID differs */
        CHECK(r.dev->open(7, bad, sizeof(bad)) == OpenResult::UnsupportedDevice);
        CHECK(std::strcmp(openResultName(OpenResult::UnsupportedDevice), "UNSUPPORTED_DEVICE") == 0);
        CHECK(fake::calls.empty() && !r.dev->format().valid());
        r.open();
        const StreamFormat &f = r.dev->format();
        CHECK(f.valid() && f.profileId == "hercules-djcontrol-inpulse-t7" && f.channels == 4 && f.sampleRate == 44100);
        CHECK(f.sampleFormat == SampleFormat::S24_3LE && f.bytesPerFrame == 12);
        r.dev->stop();
    }
    { /* usbQueuedFrames == submitted - completed and tracks the packets in flight */
        Rig r;
        r.startRunning(8000);
        for (int i = 0; i < 40; i++) r.tick();
        Stats s = r.dev->stats();
        CHECK(s.usbQueuedFrames == s.framesSubmitted - s.framesCompleted);
        CHECK(s.usbQueuedFrames >= 11u * 44 && s.usbQueuedFrames <= 13u * 45);
        r.dev->stop();
        s = r.dev->stats();
        CHECK(s.usbQueuedFrames <= s.framesSubmitted);
    }
    { /* finding 1: setLowWaterCallback after start is refused and changes nothing */
        Rig r;
        g_lw = g_lwBad = 0;
        CHECK(r.dev->setLowWaterCallback(onLow, nullptr, 1000));
        r.open();
        CHECK(r.dev->setLowWaterCallback(onLow, nullptr, 1000));
        r.feed(6000);
        CHECK(r.dev->start());
        r.verifySubs();
        CHECK(!r.dev->setLowWaterCallback(nullptr, nullptr, 0));
        for (int i = 0; i < 150; i++) r.tick(MS, false);
        r.dev->eventStep();
        CHECK(g_lw == 1); /* the original callback is still wired */
        r.dev->stop();
        CHECK(!r.dev->setLowWaterCallback(nullptr, nullptr, 0)); /* Stopped */
    }
    { /* finding 2: stop() from the low-water callback only requests; no deadlock, owner finishes */
        Rig r;
        g_stopDev = r.dev;
        g_stopCalls = 0;
        r.dev->setLowWaterCallback(stopFromLowWater, nullptr, 1000);
        r.startRunning(3000);
        for (int i = 0; i < 100; i++) r.tick(MS, false);
        r.dev->eventStep(); /* low-water fires here */
        CHECK(g_stopCalls == 1);
        CHECK(r.dev->stats().state != State::Stopped);
        r.dev->eventStep(); /* observes the request and halts */
        CHECK(r.dev->stats().reason == Halt::UserStop);
        r.dev->stop(); /* the owner's real stop */
        Stats s = r.dev->stats();
        CHECK(s.state == State::Stopped && s.totalLive == 0 && fake::pending.empty());
    }
    { /* same, with the real event thread: the callback runs on that thread and must not self-join */
        Rig r;
        r.opt.spawnThread = true;
        delete r.dev;
        r.dev = new UsbAudioOut(r.opt);
        g_stopDev = r.dev;
        g_stopCalls = 0;
        r.dev->setLowWaterCallback(stopFromLowWater, nullptr, 100000);
        r.open();
        r.feed(2000);
        fake::events_hook = [] { std::this_thread::sleep_for(std::chrono::microseconds(100)); };
        CHECK(r.dev->start());
        for (int i = 0; i < 200 && r.dev->stats().reason == Halt::None; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(g_stopCalls >= 1 && r.dev->stats().reason == Halt::UserStop);
        r.dev->stop();
        CHECK(r.dev->stats().state == State::Stopped && fake::pending.empty());
        fake::events_hook = nullptr;
    }
}

int main() {
    testGate();
    testOpenFailures();
    testExactStream();
    testShortfall();
    testDepthRaise();
    testDepthLower();
    testStopDrains();
    testErrors();
    testLowWater();
    testThreaded();
    testI2();
    CHECK(g_cb_allocs == 0); /* no allocation inside any libusb completion callback, whole run */
    PASS("usb_audio_out");
}

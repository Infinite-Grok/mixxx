/* EnginePump against UsbAudioOut on the fake libusb transport (manual event stepping). */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "check.h"
#include "fake_libusb.h"
#include "usbdirect/device_profile.h"
#include "usbdirect/engine_pump.h"
#include "usbdirect/transport_core.h"
#include "usbdirect/usb_audio_out.h"

using namespace usbdirect;

static const uint64_t MS = 1000000ull;
static uint64_t g_now = 1000 * MS;
static uint64_t fakeClock() { return g_now; }
static float sample(uint64_t n, int c) { return (float)((n * 7 + (uint64_t)c * 13) % 20000) / 40000.0f - 0.25f; }

struct Rig {
    Options opt;
    UsbAudioOut *dev = nullptr;
    uint64_t ticks = 0, verified = 0, mismatches = 0;
    size_t maxFill = 0, minFill = ~(size_t)0;
    T7Clock mirror{};
    Rig() {
        fake::reset();
        g_now = 1000 * MS;
        opt.spawnThread = false;
        opt.setPriority = false;
        opt.clock = fakeClock;
        mirror.rate = T7_NOMINAL;
        dev = new UsbAudioOut(opt);
    }
    ~Rig() { delete dev; }
    libusb_transfer *oldest(unsigned char ep) {
        for (auto *t : fake::pending)
            if (t->endpoint == ep) return t;
        return nullptr;
    }
    /* One device millisecond (+ ~1 ms real time so the pump thread can work). */
    void tick(bool checkData) {
        g_now += MS;
        ticks++;
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
        dev->eventStep(); /* low-water callback runs here */
        const size_t fill = dev->fifo().size();
        if (ticks > 20) {
            if (fill > maxFill) maxFill = fill;
            if (fill < minFill) minFill = fill;
        }
        verify(checkData);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    void verify(bool checkData) {
        for (const auto &s : fake::subs) {
            if (s.ep != 0x01) continue;
            CHECK(t7_frames(&mirror, 540) * 12 == s.length);
            for (int f = 0; f < s.length / 12; f++, verified++)
                for (int c = 0; c < 4; c++) {
                    unsigned char want[3];
                    packSample(want, (double)sample(verified, c), SampleFormat::S24_3LE);
                    if (checkData && std::memcmp(s.data + f * 12 + c * 3, want, 3)) mismatches++;
                }
        }
        fake::subs.clear();
    }
    void open() { CHECK(dev->open(7, kT7Descriptor, kT7DescriptorLen) == OpenResult::Ok); }
};

/* Deterministic render, records the frame counts it was asked for. */
struct Renderer {
    std::atomic<uint64_t> frames{0}, calls{0}, wrongSize{0}, nullBuf{0};
    uint64_t next = 0;
    unsigned expected;
    int sleepMs = 0;
    explicit Renderer(unsigned e) : expected(e) {}
    void operator()(float *buf, int n) {
        calls++;
        if (!buf) nullBuf++;
        if ((unsigned)n != expected) wrongSize++;
        if (sleepMs) std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        for (int i = 0; i < n; i++, next++)
            for (int c = 0; c < 4; c++) buf[i * 4 + c] = sample(next, c);
        frames += (uint64_t)n;
    }
};

static void testRefusals() {
    Rig r;
    Renderer rn(256);
    EnginePump p(*r.dev, std::ref(rn), 256, 2048);
    CHECK(!p.start()); /* not open: format invalid */
    CHECK(rn.calls == 0);
    r.open();
    CHECK(p.start());
    CHECK(!p.start()); /* already running */
    CHECK(r.dev->fifo().size() >= 2048); /* prefilled before the transport starts */
    CHECK(r.dev->start());
    r.verify(true);
    p.stop();
    p.stop(); /* idempotent */
    r.dev->stop();
    EnginePump late(*r.dev, std::ref(rn), 256, 2048);
    CHECK(!late.start()); /* transport stopped: callback registration refused / not open */

    Rig r2;
    Renderer rn2(256);
    r2.open();
    r2.dev->fifo().push(std::vector<float>(8 * 4, 0.f).data(), 8);
    CHECK(r2.dev->start());
    EnginePump after(*r2.dev, std::ref(rn2), 256, 2048);
    CHECK(!after.start() && rn2.calls == 0); /* low-water wiring after start is refused */
    r2.dev->stop();
}

static void testFillAndData() {
    Rig r;
    Renderer rn(256);
    r.open();
    EnginePump p(*r.dev, std::ref(rn), 256, 2048);
    CHECK(p.start());
    CHECK(rn.frames >= 2048 && rn.frames % 256 == 0);
    CHECK(r.dev->start());
    r.verify(true);
    for (int i = 0; i < 500; i++) r.tick(true);
    Stats s = r.dev->stats();
    CHECK(s.engineUnderruns == 0 && s.state == State::Running && s.reason == Halt::None);
    CHECK(r.mismatches == 0 && r.verified == s.framesSubmitted && r.verified > 20000);
    CHECK(rn.wrongSize == 0 && rn.nullBuf == 0 && rn.frames == rn.calls * 256);
    CHECK(r.maxFill <= 2048 + 256); /* never above target + one block */
    CHECK(r.minFill >= 2048 / 2);   /* refilled promptly after dropping below the low-water mark */
    CHECK(p.renderOverruns() == 0 && p.blocksRendered() == rn.calls);
    /* latency: usb queue + fifo */
    const uint64_t lat = p.latencyFrames();
    CHECK(lat >= 10u * 44 && lat <= 13u * 45 + 2048 + 256 + 45);
    r.dev->stop();
    p.stop();
    /* quiesced: exact identity */
    CHECK(p.latencyFrames() == r.dev->stats().usbQueuedFrames + r.dev->fifo().size());
    CHECK(rn.frames == r.dev->fifo().totalPushed());
}

static void testSlowRenderOverruns() {
    Rig r;
    Renderer rn(256);
    rn.sleepMs = 12; /* block is 5.8 ms: render takes twice real time */
    r.open();
    EnginePump p(*r.dev, std::ref(rn), 256, 1024);
    CHECK(p.start());
    CHECK(r.dev->start());
    r.verify(true);
    for (int i = 0; i < 300; i++) r.tick(false);
    Stats s = r.dev->stats();
    CHECK(p.renderOverruns() >= 1 && p.renderOverruns() == p.blocksRendered());
    CHECK(s.engineUnderruns >= 1 && s.engineUnderrunFrames >= 45); /* USB side zero-filled */
    CHECK(s.state == State::Running && s.errors == 0);
    CHECK(p.latencyFrames() <= 13u * 45 + 1024 + 256 + 45);
    r.dev->stop();
    p.stop();
}

static EnginePump *g_p = nullptr;
static std::atomic<int> g_stopFromRender{0};
static void testStopFromRenderIsSafe() {
    Rig r;
    Renderer base(256);
    r.open();
    EnginePump p(*r.dev, [&](float *b, int n) {
        base(b, n);
        if (base.calls == 12 && g_stopFromRender.fetch_add(1) == 0) g_p->stop(); /* request only: no self-join */
    }, 256, 2048);
    g_p = &p;
    g_stopFromRender = 0;
    CHECK(p.start());
    CHECK(r.dev->start());
    r.verify(true);
    for (int i = 0; i < 60; i++) r.tick(false);
    p.stop(); /* the real join, from another thread */
    p.stop();
    const uint64_t calls = base.calls;
    for (int i = 0; i < 20; i++) r.tick(false);
    CHECK(base.calls == calls); /* really stopped */
    r.dev->stop();
}

int main() {
    testRefusals();
    testFillAndData();
    testSlowRenderOverruns();
    testStopFromRenderIsSafe();
#ifdef USBDIRECT_USE_EVENTFD
    PASS("engine_pump (eventfd)");
#else
    PASS("engine_pump (condvar)");
#endif
}

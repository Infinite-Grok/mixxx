/* dacDelaySecs: raw value, plausibility window, substitution. */
#include <cmath>

#include "check.h"
#include "usbdirect/play_delay.h"

using namespace usbdirect;

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main() {
    DacDelayInput in;
    in.sampleRate = 44100.0;
    in.blockFrames = 256.0;
    in.expectedLatencySecs = 0.020;
    { /* normal: queued + fifo, block itself not counted */
        in.usbQueuedFrames = 528;
        in.fifoFrames = 300;
        const DacDelay d = dacDelaySecs(in);
        CHECK(!d.substituted && near(d.secs, 828.0 / 44100.0));
    }
    { /* empty: 0 is legal */
        in.usbQueuedFrames = 0;
        in.fifoFrames = 0;
        const DacDelay d = dacDelaySecs(in);
        CHECK(!d.substituted && d.secs == 0.0);
    }
    { /* exactly 2x expected is still plausible */
        in.usbQueuedFrames = (uint64_t)(0.040 * 44100.0);
        in.fifoFrames = 0;
        CHECK(!dacDelaySecs(in).substituted);
    }
    { /* implausibly large, no previous value: clamped to 2x expected */
        in.usbQueuedFrames = 44100;
        in.fifoFrames = 44100;
        const DacDelay d = dacDelaySecs(in);
        CHECK(d.substituted && near(d.secs, 0.040));
    }
    { /* implausibly large with previous value: previous + block - elapsed */
        in.haveLast = true;
        in.lastSecs = 0.020;
        in.sinceLastRenderSecs = 0.004;
        const DacDelay d = dacDelaySecs(in);
        CHECK(d.substituted && near(d.secs, 0.020 + 256.0 / 44100.0 - 0.004));
    }
    { /* substituted value is clamped to [0, 2x expected] */
        in.lastSecs = 0.039;
        in.sinceLastRenderSecs = 0.0;
        CHECK(near(dacDelaySecs(in).secs, 0.040));
        in.lastSecs = 0.001;
        in.sinceLastRenderSecs = 0.5;
        CHECK(dacDelaySecs(in).secs == 0.0);
    }
    { /* no expected latency: no upper bound */
        in.expectedLatencySecs = 0.0;
        in.usbQueuedFrames = 44100;
        const DacDelay d = dacDelaySecs(in);
        CHECK(!d.substituted && near(d.secs, 2.0));
    }
    { /* bad sample rate */
        in.sampleRate = 0.0;
        CHECK(dacDelaySecs(in).secs == 0.0);
    }
    PASS("play_delay");
}

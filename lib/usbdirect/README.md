# usbdirect: direct USB audio transport library (I1)

Host-testable C++17 port of the G5 probe transport (`tools/android-usb-probe`, commit
`4f9f03e`) for the exact T7 profile: full-speed IF1 alt1, 44100 Hz, 4 ch, signed 24-bit LE,
async feedback on EP 0x81, OUT on EP 0x01. Plan: `docs/specs/android-A4-mixxx-usb-direct-plan.md`.
Spec: `docs/specs/android-A4-usbdirect-I1-transport-lib.md`. No Mixxx changes, no device use.

## Layout

| File | Role |
| --- | --- |
| `include/usbdirect/transport_core.h` | Pure port of `stream_core.h` (clock, Q14 feedback, packet sizes, s24 pack, lifecycle, gap stats) |
| `include/usbdirect/spsc_float_fifo.h` | Wait-free SPSC ring of interleaved 4-ch float frames |
| `include/usbdirect/depth_controller.h` | Pure adaptive OUT queue depth policy |
| `include/usbdirect/usb_audio_out.h`, `src/usb_audio_out.cpp` | The class that owns libusb and the transfers |
| `include/usbdirect/stream_format.h` | `StreamFormat`, `SampleFormat`, `ChannelRole`, and `packFrames` (S16LE / S24_3LE / S32LE by channel count) |
| `include/usbdirect/device_profile.h`, `src/device_profile.cpp` | Profile registry (VID/PID + exact raw descriptor); owns the 288-byte T7 allowlist |
| `include/usbdirect/engine_pump.h`, `src/engine_pump.cpp` | `EnginePump`: engine thread that renders blocks into the FIFO |
| `tests/` | Host suites plus `fake_libusb.cpp` (link-time libusb fake) |
| `test.sh`, `crosscompile.sh`, `CMakeLists.txt` | Host tests (ASan+UBSan, -Werror), arm64 NDK check, standalone CMake |

## API (`usbdirect::UsbAudioOut`)

```
Options o; UsbAudioOut out(o);
out.open(fd, rawDescriptors, 288)      // gate first, then init/wrap/claim IF1/alt1/alloc
out.fifo().push(frames, n)             // engine thread (producer); prefill before start()
out.setLowWaterCallback(fn, user, n)   // before start()
out.start()                            // 2 feedback + depth OUT transfers, event thread
out.stats()                            // seqlock-consistent snapshot, any thread
out.stop()                             // blocks until every transfer is reaped, then cleanup
```

`open` returns an `OpenResult`; descriptor mismatch touches no USB call at all. `start()` failing
still requires `stop()`. After a halt (`stats().reason != None`, e.g. `NoDevice`) the owner must
call `stop()` to reap and release the fd. The destructor calls `stop()`.

## Threading

- **Event thread** (spawned by `start()`; `nice -19` on Android, optional CPU mask and ADPF hint
  session, all recorded in `stats()`): runs `libusb_handle_events_timeout`, which runs the
  completion callbacks. All transfer state, the feedback clock, gap stats and the depth
  controller are touched only here (and by `start()`/`stop()` while no thread runs).
- **Engine thread**: only `fifo().push()`. Wait-free, no locks, no allocation.
- **Any thread**: `stats()` (seqlock over relaxed atomics: the block is coherent, e.g.
  `packetsSubmitted == outCompleted + outLive`), `fifo().size()`.
- **Completion callbacks**: no allocation, no logging, no locks. They pop at most 45 frames from
  the FIFO, pack s24 into the slot buffer, resubmit, and publish counters. The test binary
  replaces `operator new` and asserts zero allocations inside callbacks.
- **Low-water callback**: invoked from the event loop step (not inside libusb's callback) while
  FIFO fill < threshold, once per loop iteration; it must be cheap (eventfd write).
- `Options::spawnThread=false` lets tests (or a caller) drive `eventStep()` themselves.

## Invariants

1. Exact descriptor gate (288 bytes) before any USB call; speed must be full; only IF1 is
   claimed; detach only after `BUSY` with an active driver, one retry; never reset, set
   configuration or write rates.
2. Output packets are 1 ms, one iso packet per transfer, sizes from the exact 441/10 nominal /
   Q14 feedback clock (phase kept across updates); ordinary OUT success needs
   `actual == requested`; invalid non-empty feedback, 250 ms startup without 3 valid packets, or
   128 ms staleness halt streaming.
3. Each OUT completion pops exactly its packet's frames from the FIFO. Shortfall: the frames that
   exist are consumed, the rest zero-filled, `engineUnderruns++` (distinct from USB gap
   candidates `gapExceedDepth`).
4. Stop order: halt -> cancel all -> reap all (watchdog flag after 2 s, never frees a live
   transfer) -> free transfers -> alt0 -> release IF1 -> reattach only if this object detached
   (result recorded in `ownership*`) -> driver status -> close -> exit.

## Depth control and how it changes without packet gaps

`DepthController`: start 12, clamp [4,16]. A completion gap strictly greater than the current
depth in ms, or no other OUT transfer queued (missing slot), raises immediately to
`min(16, max(depth+2, ceil(gap_ms)+2))`. After a 120 s window with no change and at least 10000
gaps (p99.99 known), the target is `ceil(maxGap_ms + 1)`; depth drops by one per window, never
below the target.

All 16 OUT transfers are allocated once; `depth` of them are in flight, the rest are parked.
Every OUT packet is produced by one function (`submitOut`) in submission order, so the packet
stream (clock phase and FIFO samples) is strictly sequential regardless of how many transfers
are in flight:

- **Raise**: inside the completion callback, after the finished slot is parked, parked slots are
  submitted until `outLive == depth`, each taking the next packet. Latency grows by the extra
  packets, but no packet is skipped or repeated.
- **Lower**: the finished slot is simply not resubmitted (retired to the parked list); the stream
  continues from the remaining transfers.

Tests compare every submitted byte to an independent reference stream and replay the clock
packet sizes across raise and lower.

## Tests

`bash tools/usbdirect/test.sh` (WSL; g++ 13, `-Werror`, ASan+UBSan, `nice 10`):
`transport_core_test` (probe `stream_test` parity), `fifo_test` (incl. 10M-frame SPSC stress),
`depth_test`, `usb_audio_out_test` (fake libusb, includes a real event thread run).
`bash tools/usbdirect/crosscompile.sh`: NDK 27.2, API 26, `-Werror`, links against the probe's
libusb 1.0.29 archive; no APK. Overrides: `LIBUSB_PREFIX`, `ANDROID_SDK`, `ANDROID_NDK_HOME`.

Software tests establish no hardware compatibility, routing, latency or Android recovery.
libusb is LGPL-2.1; the probe's `LICENSE-NOTICE.md` applies to any build that links it.

## I2a: controller-agnostic format, profile registry, EnginePump

Spec: `docs/specs/android-A4-usbdirect-I2a-engine-pump.md`.

- `StreamFormat` (`sampleRate`, `channels`, `sampleFormat`, `bytesPerFrame`, `channelRoles[]`, `profileId`,
  `displayName`); `UsbAudioOut::format()` returns it after `open()` (invalid before).
- `DeviceProfile` registry: `findProfile(raw, len)` matches VID/PID (from the device descriptor) plus the
  exact raw bytes; `profiles(&n)` enumerates. Only entry: `hercules-djcontrol-inpulse-t7` (288 bytes moved from
  the old `profile.h`, identical), transport `AsyncExplicitFeedbackFS`, no quirks. `open()` of any other
  device returns `OpenResult::UnsupportedDevice` (alias `DescriptorMismatch`) before any USB call. The
  transport itself is still the T7 one (4 ch, 540-byte packets); other formats exist at the packer level only.
- Packing is `packFrames(sampleFormat, channels)`; the T7 path is byte-identical to the I1 `t7_pack24`
  reference (tested over random and out-of-range samples, plus the I1 exact-stream suite).
- `EnginePump(out, render, framesPerBlock, targetFillFrames)`: `start()` registers the low-water callback
  (so it must precede `out.start()`), prefills to the target, then runs the engine thread (eventfd on Android,
  condition variable on host, 5 ms timeout); `stop()` joins. `latencyFrames() = usbQueuedFrames + fifo fill`,
  `renderOverruns()` counts blocks whose render exceeded the block duration. Shut down with `out.stop()` then
  `pump.stop()`. A slow render leaves the FIFO short and the USB side zero-fills (`engineUnderruns`).
- Contract fixes from the I1 review: `setLowWaterCallback` returns false (changes nothing) once the transport
  runs; `stop()` called from the low-water callback only requests the stop (no self-join), the owner calls
  `stop()` again; `EnginePump::stop()` from `render` likewise only requests.
- `Stats` gained `framesCompleted` and `usbQueuedFrames`. `SpscFloatFifo` takes a runtime channel count (default 4).

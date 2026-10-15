# USB Direct sound device (test branch `feature/usb-direct`)

Experimental. Android only. Test identity: applicationId `io.github.infinitegrok.mixxxpreview.usbdirect`,
label "Mixxx USB Direct (test)", installs next to the public preview. Not for publication.

## What it does

A new sound API "USB Direct" streams the Mixxx engine output straight to a USB audio device over
isochronous transfers (libusb on the fd from `UsbDeviceConnection`), bypassing Android's audio stack.
Oboe stays the default API; USB Direct is opt-in in Preferences > Sound hardware > Sound API.
Only the Hercules DJControl Inpulse T7 profile exists today (44100 Hz, 4 ch, s24le, async feedback;
channels 1-2 Main, 3-4 Headphones). Unknown devices match nothing and no USB call is made for them.

## Files

| Path | Role |
| --- | --- |
| `lib/usbdirect/` | Vendored library, byte-for-byte from VectorSTEMS commit named in `SOURCE.txt` |
| `src/soundio/sounddeviceusbdirect.{h,cpp}` | `SoundDeviceUsbDirect` (SoundDevice), JNI permission/open, engine render, halt handling |
| `src/soundio/portaudioenumerator.*` | Lists matching attached devices under API "USB Direct" (mutex-guarded vector) |
| `src/soundio/soundmanager.*` | `getSampleRates("USB Direct")` = profile rate; `deviceFailed()`; refresh before listing |
| `src/soundio/soundmanagerconfig.*` | `kAPIUsbDirect`, role-based default routing (Main/Headphones) |
| `res/qml/Settings/AudioRouter.qml` | friendly name for the new API |
| `CMakeLists.txt` | Android-only `usbdirect` static target + test identity |

## Threading and lifecycle

- `open()` (main thread): find UsbDevice by VID/PID, request permission if needed (polls
  `hasPermission`), `openDevice`, raw descriptors + fd, `UsbAudioOut::open` (exact descriptor gate),
  create `EnginePump`. Start order: `pump.start()` (wires low-water, prefills the FIFO with SILENCE
  on the opening thread so the Mixxx engine is never driven from the GUI thread), then `out.start()`.
- Engine thread (pump, nice -19): runs the same sequence as `SoundDevicePortAudio::callbackProcessClkRef`
  (underflow bookkeeping, readProcess, `onDeviceOutputCallback`, `composeOutputBuffer`, writeProcess),
  sets flush-to-zero once, publishes callback-to-DAC time from `usbQueuedFrames + fifo.size()`.
  The USB event thread only calls the cheap low-water thunk; libusb callbacks stay allocation/log/lock-free.
- `close()`: `UsbAudioOut::stop()` returns fully, then `EnginePump::stop()` joins, then the pump and
  transport are destroyed, then the Java `UsbDeviceConnection.close()`. Nothing is stopped from
  render/low-water callbacks. A halt seen by render (detach, errors) is only flagged; one queued
  notification runs `onHalted()` on the main thread, which closes everything via `SoundManager::deviceFailed()`
  (error device set, sound status disconnected) and shows a toast. Background does not stop playback.
- If the kernel driver could not be re-attached after stop, a toast says "Replug the controller to
  restore normal Android audio." No USB resets.
- FIFO target = block + 256 frames (override `[Android],usb_direct_fifo_target_frames`); the library's
  adaptive USB queue depth (start 12 ms, 4..16) is on.

## Limits

- T7 profile only. Sample rate list for the API = 44100 only.
- Devices are re-scanned when the API list or its devices are requested (main thread); there is no
  hotplug event. A device that is open is never dropped from the list.
- After a halt there is no automatic fallback to Oboe: audio stops until the user re-applies the sound
  configuration (or restarts). Oboe/Android audio to a replugged device may need a physical replug.
- Not yet validated on a device (no adb in this task). Thread affinity: see "I3a" below.

## I3a: thread placement and one-shot timer log (2026-10-15)

Evidence: `docs/research/android-usbdirect-blu-stalls-20261015.md` F1 (every thread created after
start-up inherited a CPU-4 pin, a little core on the BLU B160V) and F2 (one-shot timer debug line).

- `PortAudioEnumerator`: the CPU-4 pin around `Pa_Initialize()` is kept, but the calling thread's mask is
  saved first (no pin if saving fails) and restored right after `Pa_Initialize()`. One `qInfo` line.
- `usbdirect/cpu_capacity.h`: `bigCpus()` reads `/sys/devices/system/cpu/cpuN/cpu_capacity`: the max-capacity CPUs,
  plus the next tier if the top tier is a single CPU. Empty (no pinning) if any file is missing/unparseable.
- Automatic placement: with >= 2 big CPUs the USB event thread takes the highest-numbered one and the pump the
  next; with exactly 1 (or none) neither is pinned. `EnginePump` has a new `cpuMask` ctor argument and
  `affinityRc()` (same semantics as `Options::cpuMask`; failure is recorded, never fatal). The event thread
  also requests an ADPF hint session.
- Overrides under `[Android]` in the user config: `usb_direct_event_cpu_mask`, `usb_direct_pump_cpu_mask`
  (decimal or `0x` hex, CPUs 0..7; -1 = automatic, 0 = no pinning) and `usb_direct_adpf` (default 1).
- The masks, affinity/priority return codes and ADPF state are in the `UsbDirect` logcat stats line
  (`eventCpuMask= pumpCpuMask= eventAffRc= pumpAffRc= prioRc= adpf= adpfState=`) and once at open.
- `ControllerScriptInterfaceLegacy::beginTimer`: the "Starting timer" debug line is formatted only for
  repeating timers, and for one-shot timers only with controller debug on. Manual check: run a mapping with
  one-shot timers with and without `--controller-debug`; the log shows "Starting one-shot timer" only with it.


## I3b: isolation, play position, permission reapply, idle UI, analyzers (2026-10-16)

Evidence: `docs/research/android-usbdirect-blu-stalls-20261015.md` (F1, F3, F4). Each numbered change is its own commit.

1. **Core isolation.** While a USB Direct device is open, every thread of the process except the USB event thread
   and the engine pump is restricted to `allowed & ~(event | pump)` (the masks actually in force on the two
   threads, read back with `sched_getaffinity`; `allowed` is the main thread's mask). Skipped when either thread is
   not pinned or no CPU would be left. The main thread is set first (threads created later inherit it), then
   `/proc/self/task/*` is swept (QML render thread, readers, analyzers, Binder/Oboe threads); the sweep repeats on the
   10 s stats tick, and on close everything gets `allowed` back. `usbdirect/cpu_isolation.h` has the pure
   arithmetic (`planIsolation`) and the sweep; `UsbAudioOut::eventThreadTid()` and `EnginePump::threadTid()`
   give the tids to skip. Config `[Android] usb_direct_isolate` (default 1, 0 = off). The stats line gains
   `isolate= moved= failed=`; one `core isolation:` line at open logs the masks in hex. Also: the
   PortAudioEnumerator pin log resets to decimal after the hex mask.

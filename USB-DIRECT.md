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
2. **VisualPlayPosition reference.** `usbdirect/play_delay.h` (`dacDelaySecs`, host-tested) computes the delay of
   the block being rendered to the DAC: at the entry of `render()` that block is not in the FIFO yet, so it is
   `(usbQueuedFrames + fifoFrames) / sampleRate` (the block itself is not counted; the USB snapshot can lag by at most
   one packet, about 1 ms, and no device-side constant is added without measurements). The reference timer is
   restarted at the same point. The value must lie in `[0, 2 x expected latency]` (the `output_latency_ms` value);
   otherwise the previous value, advanced by one block minus the time since the previous render, is used (the
   PortAudio fallback idea). `VisualPlayPosition` logs the "outdated position request" and "no transport" warnings at
   most once per 10 s per kind for all devices, with the number of suppressed warnings.
3. **Reapply after the USB permission grant.** `controllers/androidusbpermission.h` adds one listener that
   `usbDeviceAccessResult` (Android UI thread) calls with the device's vendor/product id. `SoundManager` registers it:
   on a grant for a USB Direct profile device it queues `reapplyAfterUsbPermission()` to the main thread. That calls
   `setupDevices()` once, only if the configured API is USB Direct, no sound device is open and no setup is running
   (`m_setupInProgress`, set by a guard in `setupDevices()`); nothing happens on denial. `setupDevices()` alone is used
   instead of `clearAndQueryDevices()` + `setupDevices()` or `setConfig()`: a grant does not change the device list, and
   `clearAndQueryDevices()` closes devices and sleeps 5 s on the main thread if any is open, while `setConfig()` also
   runs `checkConfig()` and rewrites the config file. The 3 s permission poll in `open()` stays as the fast path.
4. **Idle waveforms.** `QmlWaveformDisplay::slotFrameSwapped` no longer re-arms the frame loop unconditionally.
   (a) With no track (and no static track) the loop stops after the frame that cleared it; loading a track, options,
   size changes etc. already call `update()`. (b) With a track whose engine play position and zoom equal those of the
   last frame this item drew, no frame is scheduled; instead a 50 ms `QTimer` only compares the state (no render)
   and starts the normal loop as soon as it changes (play, seek, scratch, zoom), and one frame is drawn every 200 ms
   as a heartbeat. The heartbeat is a deliberate deviation from "no frame at all": cue/loop edits, rate changes and
   analysis progress have no `update()` hook while paused. After an idle period the frame-time references are reset
   so the first frame does not trigger the position warnings. This applies on desktop too (the QML waveform item is
   shared). `[Waveform] FrameRate` defaults to 30 on Android when the key is absent (an explicit value wins).
5. **Analyzer priority and deferral.** On Android `WorkerThread::run()` also sets the nice value of workers with
   Low/Lowest/Idle priority to 10/15/19 (Qt's `QThread::setPriority` has no effect on `SCHED_OTHER` threads there); in
   practice this is the batch analysis (`AnalysisFeature` scheduler, `LowPriority`), not the load-time analysis of
   `PlayerManager`. `AnalysisFeature` watches `[ChannelN],play` for every deck (the count read when the scheduler is
   created) and `suspend()`s the batch scheduler while any deck plays; it `resume()`s it when all stopped.
   `resumeAnalysis()` (also called by `Library` when the load-time analysis goes idle) is a no-op while deferred.
   Config `[Android] defer_analysis_while_playing` (default 1, 0 = off).

## I3c: USB permission broadcast delivery (2026-10-16)
The permission PendingIntent is now an explicit intent (setPackage) with FLAG_MUTABLE, so Android 14+ delivers it to the RECEIVER_NOT_EXPORTED receiver and the system can add EXTRA_DEVICE / EXTRA_PERMISSION_GRANTED. Receiver logs "Received" at info level; typed getParcelableExtra on API 33+.

## I3d: keep cpu0 free for the USB interrupt (2026-10-16)
Isolation now leaves cpu0 (where the xHCI interrupt fires) to the interrupt path. Automatic other mask = allowed & ~audio & ~cpu0 when at least 2 CPUs remain, else the I3b value. [Android] usb_direct_isolate_other_mask: -1 (default) = automatic, else a hex/decimal mask applied as mask & allowed & ~audio (empty result skips isolation and logs why). Log and stats show the applied other= mask.

## I3e: audio threads share the whole big-core set (2026-10-16)
Automatic event and pump masks are now both the whole big-core set (when it has at least 2 CPUs; with 1 big CPU nothing is pinned). Single-core pins made the threads wait behind kswapd/kcompactd/GC that EAS moves onto the big cores under isolation. The isolation audio mask is still the union of the two applied masks (= the big set), so other threads stay off it; cpu0 rule unchanged. Explicit ini values (usb_direct_event_cpu_mask / usb_direct_pump_cpu_mask) behave as before, so single-core pins stay available for A/B. The start log line prints the masks as applied.

## I4
versionName 0.1.0-usbdirect.7: 8 samplers on Android, Developer tools window created on demand. No lib/usbdirect change.

## I5
versionName 0.1.0-usbdirect.8: T7 platter script CPU cut, behaviour identical (one decay timer per deck re-checking a deadline, lookup tables instead of regex/template strings, cached track_loaded, reused placement/tick arrays). Script only; test app only.

## I6
allshader::WaveformRenderMark::updateMarkImage/updateEndMarkImage now clear Graphics::m_obsolete after updateTexture(); previously mark images were rasterised on every frame once flagged.

## I6b
versionName 0.1.0-usbdirect.10. Test instrumentation only: (1) `src/waveform/renderers/markimagestats.h` counts `setNeedsImageUpdate` calls by cause (breadth, level, text, color, jump, other) and real main/end image regenerations per renderer label (`[ChannelN]/main|slip|resize|cues|marks`), and logs one `MarkImageStats 10s:` qInfo line per 10 s when something was counted. (2) On Android `main()` sets `QSG_INFO=1` (Qt logs the chosen render loop) and, if `<app files dir>/qsg_render_loop` exists containing `threaded` or `basic`, sets `QSG_RENDER_LOOP` before the QGuiApplication is created; `QmlApplication` logs "Qt Quick render loop: BASIC/THREADED" from `sceneGraphInitialized`.

## I6c
versionName 0.1.0-usbdirect.11. Mark breadth is single-sourced (`WaveformRenderMarkBase::markBreadth()`, overridden by `allshader::WaveformRenderMark`; used by `onResize()` and `update()`). `QmlWaveformDisplay`: read accessors `renderers_count/at` no longer flag a full renderer-stack rebuild, identical `setOptions` is a no-op, and every rebuild trigger is counted in the `MarkImageStats` line as `[ChannelN]/dirty:<reason>` (`qt-node-reset` = Qt passed no node); mark construction is labelled `[ChannelN]/create`.

## I6d
versionName 0.1.0-usbdirect.12. QmlWaveformDisplay keeps its paint-node root in members (pointer identity instead of dynamic_cast on children) and counts every way Qt can drop it in the MarkImageStats line as [ChannelN]/node:<reason> (null, foreign, release-resources, scene-in, scene-out, visible, hidden, parent).

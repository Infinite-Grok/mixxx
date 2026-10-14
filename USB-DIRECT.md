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
- Not yet validated on a device (no adb in this task). Thread affinity: the pump thread inherits the
  creating thread's CPU mask.

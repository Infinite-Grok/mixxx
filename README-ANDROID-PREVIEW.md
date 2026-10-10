# Mixxx Preview (unofficial) for Android

This branch (`android-preview`) is a public work-in-progress fork of [Mixxx](https://github.com/mixxxdj/mixxx)
that puts several pieces of Android work together so people can try them before they are adopted upstream.
It is **not an official Mixxx release, not endorsed by the Mixxx team, and not supported by them**.
Do not report problems with this build to the Mixxx project; report them here (see "Reporting").

The app is called **Mixxx Preview (unofficial)** and installs next to any other Mixxx build:
applicationId `io.github.infinitegrok.mixxxpreview`, its own settings and its own QML folder. It does not
touch or replace `org.mixxx` or any other Mixxx install. Its launcher icon carries a red PREVIEW banner so it
cannot be mistaken for an official Mixxx.

## What is in it

Layers, oldest first (the git history keeps them separate):

1. Mixxx `main` at `c82897107564` plus the Android foundation (WSL build guard, Android package name hooks).
2. A repaired PortAudio for Android (see "Building"): the Oboe backend's device registration copied a name
   one byte past its allocation. The repair is applied to the dependency library, not to Mixxx source.
3. Mixxx PR [#16135](https://github.com/mixxxdj/mixxx/pull/16135) by **pri-yan-shu**: the libremidi controller
   backend (merged at its head `3a9f20fa1`).
4. Android enablement for that backend (build libremidi's AMidi backend, ship its Java classes, declare the MIDI feature).
5. Fixes found while testing with a real controller: input/output hotplug, SysEx on the controller thread,
   nanosecond timestamps, QML controller preferences (MIDI devices get the MIDI mappings, the mapping dropdown
   no longer rewrites the mapping file), clean shutdown, Back button behaviour (Back closes the open page and
   asks before quitting).
6. libremidi (vendored in `lib/libremidi`) with our Android backend patches 0001-0007. They are prepared as
   separate local branches for upstream review; publication is pending.
7. Preview packaging: package identity, version, the public Hercules DJControl Inpulse T7 beta mapping
   (`res/controllers`, built in), release signing and build scripts (`tools/android-preview/`).

## Status and evidence (honest summary)

Tested by hand by one person on one device: Samsung Galaxy Z Fold7, Android 17, with a Hercules DJControl
Inpulse T7 over USB (class-compliant MIDI via libremidi), earlier build C5. Results:

- Controller input and output through libremidi: pads, faders, EQ, browse, play/cue, platter, lamps, FX lights
  (the T7 SysEx round trip works).
- Hard platter spinning for 15 s: no dropped connection.
- Unplug and replug while Mixxx runs (controller preferences closed): the controller and audio come back.
- Cold start: the controller delivers input at start (12 of 12 starts with the SysEx latch fix; 10 of 12 failed without it).
- Playback continues in the background.

Back button: on the main screen (Back asks before quitting) this was tested on the device. The later fix for a crash
on Back inside Preferences (Back now closes Preferences; Preferences also got a Done button) was checked on source
only. Not yet shown on a device: the signed release build itself, AAC/M4A playback with the new decoder, other
phones, other controllers, other Android versions, long sessions, phone calls and other audio interruptions.
Nothing here is a statement about audio latency or quality.

## First-run setup in the current source

When there is no saved sound configuration, Android chooses the Oboe output. If a USB device
offers at least four output channels, it assigns Main to channels 1–2 and Headphones to 3–4;
otherwise it uses the first stereo output for Main. A controller with no stored controller
choice can select and enable its one matching bundled MIDI mapping at startup. Existing sound
and controller choices are intended to remain intact. These changes were built in rc4, but
an independent upgrade-preservation review and the T7 replug check are still pending.

## Known issues

- **Idle CPU:** the QML scene never goes idle (about 40 percent of one core with nothing loaded). Battery and heat.
- **Sound-device "Retry" dialog** can still appear after USB audio has already recovered from an unplug.
- **Unplugging a controller while its Preferences page is open** is not handled (inherited from #16135).
- **Back** inside some popups other than Preferences may do nothing instead of closing the popup.
- **HID and bulk-only controllers are not supported on Android yet.** Only class-compliant USB MIDI controllers work
  (through Android's MIDI API). The HID/bulk code compiles in but is untested.
- **No FDK-AAC.** The preview contains no Fraunhofer FDK AAC code (its licence is considered GPL-incompatible).
  M4A/AAC files are decoded by FFmpeg's own LGPL AAC decoder. Mixxx's optional AAC/HE-AAC recording and
  broadcasting encoder needs a user-installed libfdk-aac and is therefore unavailable (the other recording
  and broadcasting formats are unaffected). AAC playback through the native decoder has been checked in the build (codec symbols), not yet
  on a device: M4A files are untested on hardware.
- Preview quality: the new QML interface is work in progress upstream, and expect rough edges.
- A one-off crash in Qt SQL static destructors was seen at exit in a build before the clean-shutdown fix; if you
  see a crash on Quit, please report it.
- Debug logging is off by default. Add `--controller-debug` only when asked.

## Install, update, uninstall

- Needs Android 12 (API 31) or newer, arm64, and a device that supports Android MIDI (USB host).
- Download `mixxx-preview-<version>.apk` from the Releases page, check its SHA-256 and signing certificate
  against the release notes, then install it (allow installs from your browser or file manager when asked).
- Updates install over the previous preview as long as it is signed with the same key (the certificate
  SHA-256 is in each release's notes). If the key ever changes, uninstall first (settings are lost).
- The app asks for storage access (music library, and its own folder `/storage/emulated/0/Mixxx-io.github.infinitegrok.mixxxpreview`).
  See "Permissions" below.
- Uninstall like any other app. Its folder in shared storage is not removed by Android; delete it by hand if you want.
- Controllers: Mixxx's built-in mappings are included. The **Hercules DJControl Inpulse T7** beta mapping is
  built in too (Preferences > Controllers, pick "Hercules DJControl Inpulse T7"). Other controllers: use a built-in
  mapping that matches yours.

## Permissions

The preview keeps the permission set of the Android build of Mixxx it is based on, and asks for:

- **All files access** (`MANAGE_EXTERNAL_STORAGE`, plus `WRITE_EXTERNAL_STORAGE`): Mixxx scans your music library
  folders, which are anywhere on shared storage, and keeps its settings, library database and recordings in its own
  folder there. Android shows this as a special "all files access" switch, not a normal permission prompt.
- **Microphone** (`RECORD_AUDIO`): for Mixxx's microphone and auxiliary inputs (talkover, vinyl control, line-in).
  Nothing is recorded unless you enable an input.
- **Network** (`INTERNET`, `ACCESS_NETWORK_STATE`): for Mixxx's optional features (live broadcasting, online
  lookups). Nothing is sent unless you use such a feature.
- **Audio settings, wake lock, USB** (`MODIFY_AUDIO_SETTINGS`, `WAKE_LOCK`, `USB_PERMISSION`): playback in the
  background and USB audio and MIDI controllers (USB host is required).

You can refuse or revoke any of these in Android's settings; features that need them then do not work.

## Reporting

Use the issue forms in this repository: *Controller report* (what works or does not with your controller) and
*Bug report*. Include device, Android version, controller and mapping, and a log (the forms explain how).
There is no Zulip or upstream channel for this build yet.

## Building

Short version (details and the reason for each step are in [`tools/android-preview/README.md`](tools/android-preview/README.md)):
build under Linux or WSL Ubuntu, with the Android SDK and NDK 27.2.12479018, Java 17, CMake, Ninja, and about 40 GB free.
The pinned Mixxx dependency archive contains a `libportaudio.a` without the registration repair, so the scripts
rebuild that one library from source with the repair and put it into your own extracted copy of the archive.
The same archive builds FFmpeg with the Fraunhofer FDK AAC library, which cannot be redistributed with GPL code, so a
second step rebuilds FFmpeg without it (about 10 minutes) and removes FDK-AAC from your copy.

```sh
export PREVIEW_WORK=$HOME/mixxx-preview-work
cd tools/android-preview
bash 01_prepare_deps.sh          # download + sha256-check + extract the pinned dependency archive
bash 02_build_portaudio.sh       # rebuild PortAudio with the repair
bash 03_install_portaudio.sh     # put it into your extracted copy (the zip is never modified)
bash 03b_ffmpeg_no_fdk.sh        # rebuild FFmpeg without FDK-AAC (native AAC decoder), remove fdk-aac from your copy
bash 04_configure_build.sh       # CMake + Ninja, several hours on a laptop
# your own signing key (see tools/android-preview/README.md):
PREVIEW_KEYSTORE=... PREVIEW_KEYSTORE_PASSWORD_FILE=... bash 05_assemble_release.sh
bash 06_verify_apk.sh "$PREVIEW_WORK/out/mixxx-preview-0.1.0-preview.1.apk"
```

Releases are signed with the maintainer's private key, which is not in this repository. Your own build gets your
own signature and cannot update an official preview install (uninstall first).

## Credits

- The **Mixxx team** and contributors: Mixxx itself, the QML interface, and the Android work, in particular
  **acolombier**'s Android and new-UI work that this fork builds on.
- **pri-yan-shu**: the libremidi controller backend, Mixxx PR #16135.
- **jcelerier**: [libremidi](https://github.com/celtera/libremidi) and its Android backend.
- **Hans Petter Selasky** and the PortAudio contributors: the PortAudio Oboe backend (PortAudio PR 1084) and PortAudio.
- Infinite-Grok: the T7 mapping, the fixes in this fork, and the packaging. Developed with AI assistance (Claude)
  plus human hardware testing on a Galaxy Z Fold7 and a Hercules DJControl Inpulse T7.

## Licence

Mixxx is GPL-2.0-or-later (`LICENSE`, `COPYING`). The APK as a whole is distributed under **GPL-3.0-or-later**: Mixxx's
"or any later version" allows it, and it is needed because the APK links libkeyfinder (GPL-3.0-or-later). The files in
this fork keep their own licences; the shipped third-party components are listed in [`NOTICE`](NOTICE). The licence
texts and `NOTICE` are also inside the APK (assets `legal/`). The Fraunhofer FDK AAC library is not part of the APK.
The corresponding source of every release is this repository at the commit named in the release notes, together with
the scripts and patches in `tools/android-preview/`.

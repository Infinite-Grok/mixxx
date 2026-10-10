# Building Mixxx Preview (unofficial) for Android

These scripts build the arm64 release APK from a Linux or WSL Ubuntu host. They were used for local preview
candidates, including the signed rc4 test APK; no preview release has been published. Every step has been run
in that order. They are plain bash with no hidden state: all paths come from `env.sh` and can be overridden
from the environment.

## Why there are extra steps (the PortAudio repair)

Mixxx's pinned dependency archive (`mixxx-deps-2.7-arm64-android-1c20f84a.zip`, sha256
`6786c09f491166352880984842ffa6f554e974f30a3766a2392298444c1be83c`, vcpkg `mixxxdj/vcpkg@1c20f84aa1ffca2ef18a7d9c6bd7cdd1f5f2e265`)
contains a `libportaudio.a` whose Android (Oboe) backend has a heap overflow when it registers a device name:
it allocates `strlen(name)` bytes and copies the terminating NUL after them. The repair is
`portaudio/patches/0005-Fix-Oboe-registration-safety.patch`; see `portaudio/README.md`.

The preview does not patch the downloaded zip. Instead:

1. `01_prepare_deps.sh` verifies the zip's sha256 and extracts it into a tree you own.
2. `02_build_portaudio.sh` rebuilds PortAudio from the pinned source with the Mixxx overlay patches 0001-0004
   plus the repair 0005.
3. `03_install_portaudio.sh` replaces **one file**, `installed/arm64-android/lib/libportaudio.a`, in your extracted
   copy and records before/after hashes.

Result of the original candidate build: `libportaudio.a` sha256 `44182eb98f0253cf8444d3339149db4ae819e7420ea1b6c7b47b7a48940646c0`.
Earlier runs of these scripts gave different hashes, because the objects embedded the build path in assertion
strings. Since preview rc2 the script passes `-ffile-prefix-map` (build tree and dependency tree), so no build-host
path is in the library any more; the rc2 build gave `4fb1a75dd975c97dfdfe0761567d69fa4bc8f6c92f01061282b8bb64936080a6`
(same 16 archive members and the same patched source hash as before; a different toolchain may still change the hash).
The archive original is `cf42a47ad5c0a61422b86df3a6bad98148556fd0b480a49378b2e44e56614e7c`. Compare the checks in
`portaudio/README.md` (patched `pa_oboe.cpp` hash, archive members, no `strcpy` import in `pa_oboe.cpp.o`).

## Why there is also an FFmpeg step (FDK-AAC removal)

The same archive builds its FFmpeg 7.1 with `--enable-libfdk-aac` (vcpkg ports `ffmpeg` with feature
`fedora-ffmpeg-free-safe`, which pulls in `fdk-aac 2.0.3`), so `libmixxx` would statically contain Fraunhofer's FDK AAC
library, whose licence is widely considered incompatible with the GPL. Worse for a simple removal: that FFmpeg has
**no native AAC decoder at all** (the Fedora-derived decoder list leaves out `aac`), so M4A playback went through
`libfdk_aac` only.

`03b_ffmpeg_no_fdk.sh` therefore:

1. checks out `mixxxdj/vcpkg` at the archive's commit `1c20f84aa1ffca2ef18a7d9c6bd7cdd1f5f2e265`, copies its overlay
   `ffmpeg` port and applies `ffmpeg-no-fdk/0001-ffmpeg-port-no-fdk-aac.patch` (drops the fdk-aac dependency and the 0099
   patch, removes `libfdk_aac` from the decoder/encoder lists, adds the native LGPL decoders `aac` and `aac_latm`);
2. builds only `ffmpeg[avcodec,avdevice,avfilter,avformat,core,fedora-ffmpeg-free-safe,ffmpeg,swresample,swscale]:arm64-android`
   with the same overlay triplet, NDK and FFmpeg 7.1 sources (release and debug), about 10 minutes;
3. checks the result (no FDK symbols, `ff_aac_decoder`/`ff_aac_encoder` present);
4. in your extracted copy only: replaces `lib/` and `debug/lib/` `libav*.a`/`libsw*.a`, their pkg-config files and
   `share/ffmpeg/*.cmake`, keeps the originals in `$PREVIEW_WORK/ffmpeg-archive-original/`, and moves the `fdk-aac` library,
   headers, pkg-config and cmake files to `$PREVIEW_WORK/removed-fdk-aac/` so that Mixxx's `find_package(FdkAac)` finds nothing.
   Before/after sha256 values go to `$PREVIEW_WORK/ffmpeg-no-fdk-integration.txt`.

FFmpeg stores its configure line (with `--prefix=...`) in the library, so `03b_ffmpeg_no_fdk.sh` builds in a neutral
directory, `/tmp/mixxx-preview-ffmpeg` by default (`FFMPEG_BUILD_DIR` overrides it), not below your home directory.

Qt Multimedia's prebuilt `libplugins_multimedia_ffmpegmediaplugin` also embeds FFmpeg with FDK. `04_configure_build.sh`
removes it from the deployment list (and deletes any stale copy in `android-build/libs`), so Qt Multimedia uses its Android
MediaPlayer plugin, which is bundled. Mixxx plays audio through its own FFmpeg and PortAudio, so only QML media items
are affected. `06_verify_apk.sh` fails if FDK symbols or identifying strings, or that plugin, are in the APK
(set `UNSTRIPPED_MIXXX=$PREVIEW_BUILD/libmixxx_arm64-v8a.so` to also check the exact FFmpeg codec symbols).

What changes for users: Mixxx's AAC/HE-AAC recording and broadcast encoder (`EncoderFdkAac`) loads a user-installed
`libfdk-aac` with `QLibrary` at run time; on Android none is present, so that encoder reports it is unavailable (it already
did before: nothing installs the library on a phone). Playback of M4A/AAC now uses FFmpeg's native `aac` decoder.
Seek accuracy and gapless behaviour should be compared on a device.

## Prerequisites

- Ubuntu 24.04 or WSL2 Ubuntu 24.04 (on WSL give it at least 12 GB of memory; the preview was built with a 20 GB cap).
- Android SDK with platform `android-35` and build-tools `35.0.0`; NDK `27.2.12479018`; JDK 17; CMake 3.28+; Ninja;
  Python 3; git; curl; unzip. Gradle 8.14.3 is fetched by the generated Gradle wrapper.
- About 40 GB of disk. The dependency archive is 3.6 GB and extracts to about 12 GB; the build tree is about 14 GB.
- Set `ANDROID_SDK`, `ANDROID_NDK_HOME`, `ANDROID_BUILD_TOOLS`, `JAVA_HOME` if yours differ from the defaults in `env.sh`.

## Steps

```sh
export PREVIEW_WORK=$HOME/mixxx-preview-work     # everything is created below here
cd tools/android-preview
bash 01_prepare_deps.sh        # a few minutes (downloads 3.6 GB unless BUILDENV_ZIP points to your copy)
bash 02_build_portaudio.sh     # under a minute
bash 03_install_portaudio.sh
bash 03b_ffmpeg_no_fdk.sh      # about 10 minutes at 3 jobs, needs network
nohup setsid nice -n 10 bash 04_configure_build.sh > "$PREVIEW_WORK/build.log" 2>&1 &   # 25-60 min with NINJA_JOBS=3
```

Keep `NINJA_JOBS` low (default 3): each Qt/Mixxx compile job can need over 1.5 GB, and an out-of-memory kill in the
middle of a build leaves half-written objects.

### Signing key

Releases are signed with the maintainer's key, which is never in this repository. To build your own installable APK,
create a key of your own (it cannot update the official preview, which is signed by a different key):

```sh
keytool -genkeypair -keystore my-preview.keystore -storetype PKCS12 -alias mixxxpreview -keyalg RSA -keysize 4096 \
        -validity 10950 -dname "CN=My Mixxx Preview build"        # keytool asks for a password
printf '%s\n' 'the-password' > my-preview.password                  # keep this file private
export PREVIEW_KEYSTORE=$PWD/my-preview.keystore PREVIEW_KEYSTORE_PASSWORD_FILE=$PWD/my-preview.password
bash 05_assemble_release.sh      # -> $PREVIEW_WORK/out/mixxx-preview-<versionName>.apk
bash 06_verify_apk.sh "$PREVIEW_WORK/out/mixxx-preview-0.1.0-preview.1.apk"
```

`05_assemble_release.sh`:

- sets the Gradle `applicationId` to `io.github.infinitegrok.mixxxpreview` (Qt takes it from the Java namespace `org.mixxx`);
- runs `gradlew assembleRelease` (release build type: not debuggable; R8/minify is **off**, so no keep rules are needed
  and the `dev.celtera.libremidi.*` and `org.mixxx.MainActivity` native-method classes are untouched; step 6 checks that);
- `zipalign -P 16` so the stored native libraries are 16 KB aligned (the linker option `-z max-page-size=16384` in
  `CMakeLists.txt` makes the libraries themselves 16 KB aligned);
- signs with `apksigner` (v2 and v3, no v1 because minSdk is 31), reading the password only through `file:`.

`06_verify_apk.sh` checks the signature, certificate, applicationId, label, debuggable flag, SDK levels, ABI, zip and ELF
16 KB alignment of every `.so`, the JNI exports and dex classes that libremidi needs, and that the T7 mapping is in the assets.
For a public release it also checks that no file of the APK contains a private path or user name (your home directory,
the source and work directories, `/mnt/<drive>/`, Windows user paths), that the QML debugger plugins (`qmltooling`) are
absent, that `assets/legal/` has `COPYING`, `LICENSE`, `GPL-3.0.txt` and a `NOTICE` identical to the repository one, that
the launcher icon has the PREVIEW badge, and that the permissions are exactly the documented set. Give it the build log
(`BUILD_LOG=<log of 04_configure_build.sh>`) to also prove the build came from the clean, committed HEAD.

## Build hygiene

- `04_configure_build.sh` passes `-ffile-prefix-map` for the source, work, dependency and build directories (through
  `CFLAGS`/`CXXFLAGS`), so `__FILE__`/assert strings and debug info name `/mixxx-preview/...`, `/deps/...`, `/build/...`.
- It leaves the QML debugger and profiler plugins out of the deployment.
- The APK carries `COPYING`, `LICENSE`, `GPL-3.0.txt` and `NOTICE` in `assets/legal/` (`copy-resource-android` in `CMakeLists.txt`).
- Build from a clean committed tree: the build log records `Git describe` and `Git worktree modified`.

## Changing the version

`MIXXX_ANDROID_PREVIEW_VERSION_NAME` and `MIXXX_ANDROID_PREVIEW_VERSION_CODE` (CMake cache variables, defaults in
`CMakeLists.txt`). versionCode scheme: `MAJOR*1000000 + MINOR*10000 + PATCH*100 + PREVIEW`, so `0.1.0-preview.2` is `10002`.
It must only ever increase, or Android refuses the update.

## Not in these scripts

No upload, no `git push`, no `adb`. Installing is left to you.

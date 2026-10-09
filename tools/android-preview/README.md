# Building Mixxx Preview (unofficial) for Android

These scripts build the arm64 release APK from a Linux or WSL Ubuntu host. They were used for the published
preview builds; every step has been run in that order. They are plain bash with no hidden state: all paths come
from `env.sh` and can be overridden from the environment.

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

Result of the original candidate build:  sha256 . The preview rerun of these scripts (different scratch path) gave a different hash, because the objects embed the build path in assertion strings; it has the same 16 archive members and the same patched source hash. The archive original is . Compare s checks (patched  hash, archive members, no  import in ) instead of the library hash.

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

## Changing the version

`MIXXX_ANDROID_PREVIEW_VERSION_NAME` and `MIXXX_ANDROID_PREVIEW_VERSION_CODE` (CMake cache variables, defaults in
`CMakeLists.txt`). versionCode scheme: `MAJOR*1000000 + MINOR*10000 + PATCH*100 + PREVIEW`, so `0.1.0-preview.2` is `10002`.
It must only ever increase, or Android refuses the update.

## Not in these scripts

No upload, no `git push`, no `adb`. Installing is left to you.

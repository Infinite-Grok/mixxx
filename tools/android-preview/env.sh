# Shared settings for the Mixxx Preview (unofficial) Android build scripts.
# Source this file; every value can be overridden from the environment first.
#
# Layout (everything below $PREVIEW_WORK is created by the scripts):
#   $PREVIEW_WORK/deps/<BUILDENV_NAME>/   extracted dependency archive, with the repaired libportaudio.a
#   $PREVIEW_WORK/portaudio/              PortAudio source + build scratch
#   $PREVIEW_WORK/build/                  CMake/Ninja build tree
#   $PREVIEW_WORK/out/                    release APKs
# Not a Mixxx 2.7 desktop build: this only drives the Android arm64 cross build from a Linux host.

PREVIEW_SRC="${PREVIEW_SRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
PREVIEW_WORK="${PREVIEW_WORK:-$HOME/mixxx-preview-work}"

# Pinned Mixxx dependency archive (same pin as tools/android_buildenv.sh).
BUILDENV_NAME="${BUILDENV_NAME:-mixxx-deps-2.7-arm64-android-1c20f84a}"
BUILDENV_URL="${BUILDENV_URL:-https://downloads.mixxx.org/dependencies/2.7/Linux/${BUILDENV_NAME}.zip}"
BUILDENV_SHA256="${BUILDENV_SHA256:-6786c09f491166352880984842ffa6f554e974f30a3766a2392298444c1be83c}"
# If you already downloaded the zip, point to it (it is never modified).
BUILDENV_ZIP="${BUILDENV_ZIP:-$PREVIEW_WORK/${BUILDENV_NAME}.zip}"

DEPS_ROOT="${DEPS_ROOT:-$PREVIEW_WORK/deps}"
DEPS_DIR="$DEPS_ROOT/$BUILDENV_NAME"
DEPS_PREFIX="$DEPS_DIR/installed/arm64-android"

# Build-host paths must not end up in the shipped binaries (assert/__FILE__ strings, debug info). The more specific
# prefixes come last: for overlapping maps the last matching one wins.
PREVIEW_PREFIX_MAP="-ffile-prefix-map=$PREVIEW_WORK=/preview-work -ffile-prefix-map=$PREVIEW_SRC=/mixxx-preview -ffile-prefix-map=$DEPS_DIR=/deps"

# Toolchain (the versions the preview was built and verified with).
ANDROID_SDK="${ANDROID_SDK:-/usr/lib/android-sdk}"
ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_SDK/ndk/27.2.12479018}"
ANDROID_BUILD_TOOLS="${ANDROID_BUILD_TOOLS:-$ANDROID_SDK/build-tools/35.0.0}"
JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-17-openjdk-amd64}"

# Parallelism: Qt/Mixxx needs about 1.5 GB per compile job; keep this low on a laptop or in WSL.
NINJA_JOBS="${NINJA_JOBS:-3}"
GRADLE_WORKERS="${GRADLE_WORKERS:-2}"

# applicationId (must match CMakeLists.txt ANDROID_PACKAGE_NAME).
PREVIEW_APP_ID="io.github.infinitegrok.mixxxpreview.usbdirect"

# Release signing: the key is NEVER in the repository. The password is read from a file.
PREVIEW_KEYSTORE="${PREVIEW_KEYSTORE:-}"
PREVIEW_KEY_ALIAS="${PREVIEW_KEY_ALIAS:-mixxxpreview}"
PREVIEW_KEYSTORE_PASSWORD_FILE="${PREVIEW_KEYSTORE_PASSWORD_FILE:-}"

export ANDROID_SDK ANDROID_NDK_HOME JAVA_HOME

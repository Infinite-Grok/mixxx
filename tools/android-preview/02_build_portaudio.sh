#!/usr/bin/env bash
# Step 2: rebuild libportaudio.a for Android WITH the Oboe device-registration repair (see portaudio/README.md).
# Source: PortAudio b0cc303e (GitHub tarball, sha512 checked) + the Mixxx vcpkg overlay patches 0001-0004
# (PR 1084 Oboe backend) + our patch 0005 (registration buffer overflow / ownership fix).
# Needs the dependency tree from step 1 (liboboe.a, libasound.a, libjack.a, headers, pkg-config files).
set -euo pipefail
. "$(dirname "$0")/env.sh"
HERE="$(cd "$(dirname "$0")" && pwd)"
BASE=b0cc303e95fdb7c6c953337051378071c9043e88
SHA512=fa7b40604ed97c1c5157c4faeb2188e7bbbddd06752811ff8d3facd30151e2f9104737210b7df21cf0f5c0661b28eb6db9b51da09cf015924b4e56835c111c46
PATCHED_PA_OBOE_SHA256=4295e67089691aed812b09c6530894200e025d97ceeefbde788d6ad83feeb28e
PATCHED_PA_OBOE_EXP1_SHA256=7b3fbdb6e8fe8914387b08a0375997019d4a661fce4144bb6a9d309543237456
PA="$PREVIEW_WORK/portaudio"
TARBALL="${PORTAUDIO_TARBALL:-$PREVIEW_WORK/portaudio-$BASE.tar.gz}"
[ -e "$PA" ] && { echo "$PA exists; remove it to rebuild" >&2; exit 1; }
test -f "$DEPS_PREFIX/lib/liboboe.a"
mkdir -p "$PREVIEW_WORK"
[ -f "$TARBALL" ] || curl -L --fail -o "$TARBALL" "https://github.com/PortAudio/portaudio/archive/$BASE.tar.gz"
echo "$SHA512  $TARBALL" | sha512sum -c -
(cd "$HERE/portaudio/patches" && sha256sum -c ../patches.sha256)
mkdir -p "$PA/src" "$PA/logs"
tar -xzf "$TARBALL" -C "$PA/src"
SRC="$PA/src/portaudio-$BASE"
cd "$SRC"
for p in 0001-Add-basic-support-for-iOS-to-portaudio 0002-Update-CMakeLists-with-iOS-implementation \
         0003-Fix-renamed-memory-allocation-functions 0004-Add-Android-OBOE 0005-Fix-Oboe-registration-safety; do
  git apply "$HERE/portaudio/patches/$p.patch"
done
echo "$PATCHED_PA_OBOE_SHA256  src/hostapi/oboe/pa_oboe.cpp" | sha256sum -c -
git apply "$HERE/portaudio/patches/0006-Log-Oboe-output-stream-open.patch"
echo "$PATCHED_PA_OBOE_EXP1_SHA256  src/hostapi/oboe/pa_oboe.cpp" | sha256sum -c -
# Host pkg-config must not leak in (a host libsndio was auto-detected once): use the dependency tree only.
export PKG_CONFIG_LIBDIR="$DEPS_PREFIX/lib/pkgconfig:$DEPS_PREFIX/share/pkgconfig"
# ALSA and JACK stay ON: Mixxx references PaAlsa_EnableRealtimeScheduling, and the pinned dependency's own
# libportaudio.a has the same 16 members (pa_linux_alsa.c.o, pa_jack.c.o).
# Build-host paths must not end up in the library (debug line tables, __FILE__). The NDK toolchain file appends its
# own default flags to these, so they are not lost.
PA_MAP="-ffile-prefix-map=$PA=/portaudio -ffile-prefix-map=$DEPS_DIR=/deps"
cmake -S "$SRC" -B "$PA/build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=14 \
  "-DCMAKE_CXX_FLAGS=-fexceptions $PA_MAP" "-DCMAKE_C_FLAGS=$PA_MAP" \
  -DPA_BUILD_SHARED_LIBS=OFF -DPA_USE_SNDIO=OFF -DPA_USE_ALSA=ON -DPA_USE_JACK=ON \
  -DCMAKE_PREFIX_PATH="$DEPS_PREFIX" -DALSA_LIBRARY="$DEPS_PREFIX/lib/libasound.a" -DALSA_INCLUDE_DIR="$DEPS_PREFIX/include" \
  -DPA_USE_OBOE=ON -DPA_BUILD_EXAMPLES=OFF -DPA_BUILD_TESTS=OFF \
  -DOBOE_INCLUDE_DIR="$DEPS_PREFIX/include" -DOBOE_LIBRARIES="$DEPS_PREFIX/lib/liboboe.a" \
  -DCMAKE_INSTALL_PREFIX="$PA/install" 2>&1 | tee "$PA/logs/configure.txt"
nice -n 10 cmake --build "$PA/build" --parallel 2 2>&1 | tee "$PA/logs/build.txt"
cmake --install "$PA/build" 2>&1 | tee "$PA/logs/install.txt"
LLVM_AR="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar"
"$LLVM_AR" t "$PA/install/lib/libportaudio.a" | sort | tee "$PA/logs/members.txt"
sha256sum "$PA/install/lib/libportaudio.a" | tee "$PA/logs/library.sha256"

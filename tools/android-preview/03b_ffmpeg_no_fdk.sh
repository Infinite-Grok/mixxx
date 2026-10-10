#!/usr/bin/env bash
# Step 3b: replace the dependency archive's FFmpeg (built WITH Fraunhofer FDK-AAC, GPL-incompatible) by an FFmpeg
# built WITHOUT it, and remove FDK-AAC from YOUR extracted copy of the dependency tree.
#
# What is rebuilt: only the vcpkg `ffmpeg` port, from the same mixxxdj/vcpkg commit, FFmpeg 7.1, triplet
# arm64-android (overlay triplet), NDK, patches and feature set as the archive. Differences (see ffmpeg-no-fdk/*.patch):
#   - no fdk-aac dependency, --disable-libfdk-aac, patch 0099 (fdk "free" configure tweak) not applied;
#   - libfdk_aac removed from the Fedora-derived decoder/encoder lists;
#   - FFmpeg's native LGPL AAC decoders `aac` and `aac_latm` ADDED. (The archive had no native AAC decoder at all: AAC
#     decoding went through libfdk_aac only, so removing FDK without this would break M4A/AAC playback.)
# The downloaded zip is never touched. Needs network (vcpkg sources, Fedora lists) and about 10 minutes at 3 jobs.
set -euo pipefail
. "$(dirname "$0")/env.sh"
HERE="$(cd "$(dirname "$0")" && pwd)"
VCPKG_COMMIT=1c20f84aa1ffca2ef18a7d9c6bd7cdd1f5f2e265
FF="$PREVIEW_WORK/ffmpeg-native-aac"
NEW="$FF/installed/arm64-android"
REMOVED="$PREVIEW_WORK/removed-fdk-aac"
RECORD="$PREVIEW_WORK/ffmpeg-no-fdk-integration.txt"
NDK_BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
test -f "$DEPS_PREFIX/lib/libavcodec.a"
if [ -f "$NEW/lib/libavcodec.a" ]; then
  echo "reusing the FFmpeg already built in $FF (remove that directory to rebuild)"
else
[ -e "$FF" ] && { echo "$FF exists but has no build; remove it" >&2; exit 1; }
mkdir -p "$FF/logs"
git clone -q https://github.com/mixxxdj/vcpkg.git "$FF/vcpkg"
git -C "$FF/vcpkg" checkout -q "$VCPKG_COMMIT"
mkdir -p "$FF/overlay-ports"
cp -r "$FF/vcpkg/overlay/ports/ffmpeg" "$FF/overlay-ports/ffmpeg"
patch -p2 -d "$FF/overlay-ports/ffmpeg" < "$HERE/ffmpeg-no-fdk/0001-ffmpeg-port-no-fdk-aac.patch"
(cd "$FF/vcpkg" && ./bootstrap-vcpkg.sh -disableMetrics)
export VCPKG_DISABLE_METRICS=1 VCPKG_MAX_CONCURRENCY="${NINJA_JOBS}" ANDROID_NDK_HOME ANDROID_NDK_ROOT="$ANDROID_NDK_HOME"
# classic mode: run from a directory WITHOUT vcpkg.json
cd "$FF"
nice -n 10 "$FF/vcpkg/vcpkg" --vcpkg-root="$FF/vcpkg" install \
  "ffmpeg[avcodec,avdevice,avfilter,avformat,core,fedora-ffmpeg-free-safe,ffmpeg,swresample,swscale]:arm64-android" \
  --overlay-ports="$FF/overlay-ports" --overlay-triplets="$FF/vcpkg/overlay/triplets" \
  --x-install-root="$FF/installed" --x-buildtrees-root="$FF/buildtrees" --x-packages-root="$FF/packages" \
  --binarysource=clear 2>&1 | tee "$FF/logs/vcpkg-install.log"

fi

# Sanity before touching the deps copy: native AAC present, nothing FDK.
SYMS="$(mktemp)"
for lib in "$NEW"/lib/*.a "$NEW"/debug/lib/*.a; do
  "$NDK_BIN/llvm-nm" "$lib" > "$SYMS" 2>/dev/null || true
  if grep -Eq 'libfdk|aacEnc_|aacDecoder_|FDKmem' "$SYMS"; then echo "FDK symbols in $lib" >&2; exit 1; fi
done
"$NDK_BIN/llvm-nm" "$NEW/lib/libavcodec.a" > "$SYMS" 2>/dev/null || true
grep -q ' D ff_aac_decoder$' "$SYMS"
grep -q ' D ff_aac_encoder$' "$SYMS"
rm -f "$SYMS"

# Replace in the extracted copy (release + debug libs, pkg-config files, FFmpeg cmake/share files).
mkdir -p "$REMOVED" "$PREVIEW_WORK/ffmpeg-archive-original"
# keep the archive originals next to the work tree (the zip itself stays untouched as well)
for sub in lib debug/lib; do mkdir -p "$PREVIEW_WORK/ffmpeg-archive-original/$sub"; cp -a "$DEPS_PREFIX/$sub"/libav*.a "$DEPS_PREFIX/$sub"/libsw*.a "$PREVIEW_WORK/ffmpeg-archive-original/$sub/"; done
{
echo "# before (archive originals)"
(cd "$DEPS_PREFIX" && sha256sum lib/libav*.a lib/libsw*.a debug/lib/libav*.a debug/lib/libsw*.a lib/pkgconfig/libav*.pc lib/pkgconfig/libsw*.pc)
(cd "$DEPS_PREFIX" && sha256sum lib/libfdk-aac.a debug/lib/libfdk-aac.a)
} > "$RECORD"
for sub in lib debug/lib; do
  for f in "$NEW/$sub"/lib{avcodec,avdevice,avfilter,avformat,avutil,swresample,swscale}.a \
           "$NEW/$sub"/pkgconfig/lib{avcodec,avdevice,avfilter,avformat,avutil,swresample,swscale}.pc; do
    rel="${f#$NEW/}"
    cp -- "$f" "$DEPS_PREFIX/$rel"
  done
done
cp -- "$NEW/share/ffmpeg/FindFFMPEG.cmake" "$NEW/share/ffmpeg/vcpkg-cmake-wrapper.cmake" "$NEW/share/ffmpeg/vcpkg_abi_info.txt" \
      "$NEW/share/ffmpeg/vcpkg.spdx.json" "$DEPS_PREFIX/share/ffmpeg/"
# Remove FDK-AAC (moved aside, not deleted) so Mixxx's find_package(FdkAac) no longer finds it.
for rel in lib/libfdk-aac.a debug/lib/libfdk-aac.a lib/pkgconfig/fdk-aac.pc debug/lib/pkgconfig/fdk-aac.pc include/fdk-aac share/fdk-aac; do
  if [ -e "$DEPS_PREFIX/$rel" ]; then mkdir -p "$REMOVED/$(dirname "$rel")"; mv "$DEPS_PREFIX/$rel" "$REMOVED/$rel"; fi
done
{
echo "# after (FDK-free FFmpeg)"
(cd "$DEPS_PREFIX" && sha256sum lib/libav*.a lib/libsw*.a debug/lib/libav*.a debug/lib/libsw*.a lib/pkgconfig/libav*.pc lib/pkgconfig/libsw*.pc)
echo "# vcpkg: mixxxdj/vcpkg@$VCPKG_COMMIT; ffmpeg 7.1 port-version 5 + ffmpeg-no-fdk/0001-ffmpeg-port-no-fdk-aac.patch"
} | tee -a "$RECORD"
! grep -rIl 'fdk-aac' "$DEPS_PREFIX/lib/pkgconfig" "$DEPS_PREFIX/debug/lib/pkgconfig"
echo "done; record in $RECORD"

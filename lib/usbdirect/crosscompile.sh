#!/usr/bin/env bash
# arm64 Android cross-compile check (NDK 27.2, API 26, real libusb 1.0.29 headers/archive).
# Compiles the library with -Werror and checks that it links against libusb-1.0.a into a shared
# object; no APK is produced. One compiler job at nice 10.
set -euo pipefail
cd "$(dirname "$0")"
SDK=${ANDROID_SDK:-/usr/lib/android-sdk}
NDK=${ANDROID_NDK_HOME:-$SDK/ndk/27.2.12479018}
DEPS=${LIBUSB_PREFIX:-/home/pkoaw/t7-android-candidates/preview-build-responsiveness/deps/mixxx-deps-2.7-arm64-android-1c20f84a/installed/arm64-android}
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
OUT=out/android
mkdir -p "$OUT"
CXXF=(-std=c++17 -O2 -fPIC -Wall -Wextra -Werror -Iinclude -I"$DEPS/include/libusb-1.0")
OBJS=()
for s in usb_audio_out device_profile engine_pump; do
  nice -n 10 "$BIN/aarch64-linux-android26-clang++" "${CXXF[@]}" -c "src/$s.cpp" -o "$OUT/$s.o"
  OBJS+=("$OUT/$s.o")
done
nice -n 10 "$BIN/llvm-ar" rcs "$OUT/libusbdirect.a" "${OBJS[@]}"
# Header-only parts must also compile standalone under the NDK compiler.
for h in transport_core depth_controller spsc_float_fifo stream_format device_profile engine_pump; do
  printf '#include "usbdirect/%s.h"\nint main(){return 0;}\n' "$h" > "$OUT/hdr_$h.cpp"
  nice -n 10 "$BIN/aarch64-linux-android26-clang++" -std=c++17 -Wall -Wextra -Werror -Iinclude -c "$OUT/hdr_$h.cpp" -o "$OUT/hdr_$h.o"
done
# Link check: shared object with libusb static archive, same flags as the probe (16 KiB pages).
nice -n 10 "$BIN/aarch64-linux-android26-clang++" -shared -static-libstdc++ -Wl,-z,max-page-size=16384 \
  "${OBJS[@]}" "$DEPS/lib/libusb-1.0.a" -llog -lm -o "$OUT/libusbdirect_check.so"
file "$OUT/libusbdirect_check.so" "$OUT/engine_pump.o" | sed 's/, BuildID.*//'
echo "arm64 Android cross-compile OK (no APK built)"

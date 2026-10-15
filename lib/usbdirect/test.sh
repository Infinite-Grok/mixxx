#!/usr/bin/env bash
# Host tests for usbdirect: g++ -std=c++17 -Wall -Wextra -Werror, ASan+UBSan. No device, no network.
set -euo pipefail
cd "$(dirname "$0")"
DEPS=${LIBUSB_PREFIX:-/home/pkoaw/t7-android-candidates/preview-build-responsiveness/deps/mixxx-deps-2.7-arm64-android-1c20f84a/installed/arm64-android}
OUT=out/test
mkdir -p "$OUT"
CXX=${CXX:-g++}
FLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined
       -Iinclude -I"$DEPS/include/libusb-1.0")
run() { # name sources...
  local name=$1; shift
  nice -n 10 "$CXX" "${FLAGS[@]}" "$@" -pthread -o "$OUT/$name"
  "$OUT/$name"
}
run transport_core_test tests/transport_core_test.cpp -lm
run depth_test tests/depth_test.cpp
run fifo_test tests/fifo_test.cpp
run cpu_capacity_test tests/cpu_capacity_test.cpp
run cpu_isolation_test tests/cpu_isolation_test.cpp
SRC="src/usb_audio_out.cpp src/device_profile.cpp"
run device_profile_test tests/device_profile_test.cpp src/device_profile.cpp
run usb_audio_out_test tests/usb_audio_out_test.cpp tests/fake_libusb.cpp $SRC -lm
run engine_pump_test tests/engine_pump_test.cpp tests/fake_libusb.cpp $SRC src/engine_pump.cpp -lm
run engine_pump_eventfd_test tests/engine_pump_test.cpp tests/fake_libusb.cpp $SRC src/engine_pump.cpp -lm -DUSBDIRECT_USE_EVENTFD
echo "ALL usbdirect host suites passed"

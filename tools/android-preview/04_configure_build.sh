#!/usr/bin/env bash
# Step 4: configure and build the Android APK target (native code + Qt deployment), arm64-v8a only.
# Run detached, it takes 25 to 60 minutes:  nohup setsid nice -n 10 bash 04_configure_build.sh > build.log 2>&1 &
set -euo pipefail
. "$(dirname "$0")/env.sh"
B="${PREVIEW_BUILD:-$PREVIEW_WORK/build}"
export MIXXX_VCPKG_ROOT="$DEPS_DIR" VCPKG_TARGET_TRIPLET=arm64-android
# Same pins as tools/android_buildenv.sh; the archive is already extracted, so nothing is downloaded.
export BUILDENV_BASEPATH="$DEPS_ROOT" BUILDENV_NAME BUILDENV_URL BUILDENV_SHA256
export GRADLE_OPTS="-Dorg.gradle.workers.max=$GRADLE_WORKERS -Xmx2g -Dorg.gradle.java.installations.auto-detect=false -Dorg.gradle.java.installations.paths=$JAVA_HOME"
echo "BUILD start $(date -u +%FT%TZ) src=$PREVIEW_SRC commit=$(git -C "$PREVIEW_SRC" rev-parse HEAD)"
if [ ! -f "$B/CMakeCache.txt" ]; then
  # CMAKE_CXX_SCAN_FOR_MODULES=OFF: CMake 3.28 otherwise needs clang-scan-deps for the C++20 try_compile.
  nice -n 10 cmake -G Ninja -S "$PREVIEW_SRC" -B "$B" \
    -DCMAKE_TOOLCHAIN_FILE="$MIXXX_VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DCMAKE_SYSTEM_NAME=Android -DCMAKE_SYSTEM_VERSION=35 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DBULK=ON -DQT6=ON -DQML=ON -DHID=ON \
    -DVCPKG_TARGET_TRIPLET=arm64-android -DVCPKG_DEFAULT_HOST_TRIPLET=x64-linux-release \
    -DBUILD_TESTING=OFF -DBUILD_BENCH=OFF
fi
echo "CONFIGURED $(date -u +%FT%TZ)"
nice -n 10 cmake --build "$B" --target apk -j "$NINJA_JOBS"
echo "APK_TARGET_DONE $(date -u +%FT%TZ)"

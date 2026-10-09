#!/usr/bin/env bash
# Step 5: release APK = Gradle assembleRelease (non-debuggable, no R8/minify, so the libremidi and MainActivity
# JNI classes are kept as they are), zipalign -P 16 (16 KB page alignment of the stored .so files), then
# apksigner (v2 + v3 signing; minSdk 31 so no v1) with the preview release key.
# The key password must equal the keystore password (apksigner then reuses it; the same file cannot be read twice).
# Needs PREVIEW_KEYSTORE and PREVIEW_KEYSTORE_PASSWORD_FILE. The password is only passed as `file:` and is never echoed.
# Usage: 05_assemble_release.sh [outdir]   (default $PREVIEW_WORK/out)
set -euo pipefail
. "$(dirname "$0")/env.sh"
: "${PREVIEW_KEYSTORE:?set PREVIEW_KEYSTORE}" "${PREVIEW_KEYSTORE_PASSWORD_FILE:?set PREVIEW_KEYSTORE_PASSWORD_FILE}"
B="${PREVIEW_BUILD:-$PREVIEW_WORK/build}"
OUT="${1:-$PREVIEW_WORK/out}"; mkdir -p "$OUT"
cd "$B/android-build"
# Qt derives the Gradle applicationId from the Java namespace (org.mixxx), which must stay; set it here.
VN=$(sed -n 's/^MIXXX_ANDROID_PREVIEW_VERSION_NAME:STRING=//p' "$B/CMakeCache.txt")
VC=$(sed -n 's/^MIXXX_ANDROID_PREVIEW_VERSION_CODE:STRING=//p' "$B/CMakeCache.txt")
[ -n "$VN" ] && [ -n "$VC" ] || { echo "version cache variables not found in $B/CMakeCache.txt" >&2; exit 1; }
# The manifest in packaging/android carries a placeholder version, so the real one is set here as well.
python3 -I - "$PREVIEW_APP_ID" "$VN" "$VC" <<'PY'
import re
import sys

app_id, version_name, version_code = sys.argv[1:4]
p = "build.gradle"
s = open(p).read()
# idempotent: drop lines from an earlier run
s = re.sub(r'^ *(applicationId|versionCode|versionName) .*\n', '', s, flags=re.M)
old = '    defaultConfig {\n'
assert s.count(old) == 1, "unexpected generated build.gradle"
s = s.replace(
    old,
    old + '        applicationId "%s"\n        versionCode %s\n        versionName "%s"\n'
    % (app_id, version_code, version_name),
    1,
)
open(p, "w").write(s)
PY
rm -rf build/outputs/apk/release
nice -n 10 ./gradlew assembleRelease --max-workers="$GRADLE_WORKERS" -q \
  -Dorg.gradle.java.installations.paths="$JAVA_HOME" -Dorg.gradle.java.installations.auto-detect=false
UNSIGNED=$(ls build/outputs/apk/release/*-unsigned.apk)
ALIGNED="$OUT/aligned.tmp.apk"; rm -f "$ALIGNED"
"$ANDROID_BUILD_TOOLS/zipalign" -P 16 -f 4 "$UNSIGNED" "$ALIGNED"
FINAL="$OUT/mixxx-preview-$VN.apk"
"$ANDROID_BUILD_TOOLS/apksigner" sign --ks "$PREVIEW_KEYSTORE" --ks-key-alias "$PREVIEW_KEY_ALIAS" \
  --ks-pass "file:$PREVIEW_KEYSTORE_PASSWORD_FILE" \
  --v1-signing-enabled false --v2-signing-enabled true --v3-signing-enabled true \
  --out "$FINAL" "$ALIGNED"
rm -f "$ALIGNED" "$FINAL.idsig"
sha256sum "$FINAL" | tee "$FINAL.sha256"

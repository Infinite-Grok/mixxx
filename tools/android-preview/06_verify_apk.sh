#!/usr/bin/env bash
# Step 6: check a release APK. Prints PASS/FAIL lines and exits 1 on any failure. Needs no device.
# Usage: 06_verify_apk.sh <apk> [expected-cert-sha256-hex-no-colons]
set -uo pipefail
. "$(dirname "$0")/env.sh"
APK=$(realpath "${1:?apk}"); WANT_CERT=${2:-}
BT="$ANDROID_BUILD_TOOLS"; LLVM="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
fail=0
ok()  { echo "PASS $*"; }
bad() { echo "FAIL $*"; fail=1; }
chk() { local what=$1; shift; if "$@" >/dev/null 2>&1; then ok "$what"; else bad "$what"; fi; }

sha256sum "$APK"
CERTS=$("$BT/apksigner" verify --verbose --print-certs "$APK" 2>&1) || bad "apksigner verify"
echo "$CERTS" | grep -E "^Verifies|Verified using|certificate (DN|SHA-256)|Number of signers"
echo "$CERTS" | grep -q "Verified using v2 scheme (APK Signature Scheme v2): true" && ok "v2 signature" || bad "v2 signature"
echo "$CERTS" | grep -q "Verified using v3 scheme (APK Signature Scheme v3): true" && ok "v3 signature" || bad "v3 signature"
echo "$CERTS" | grep -qi "Android Debug" && bad "debug certificate" || ok "not the debug certificate"
if [ -n "$WANT_CERT" ]; then
  echo "$CERTS" | grep -qi "certificate SHA-256 digest: $WANT_CERT" && ok "signer certificate SHA-256 matches" || bad "signer certificate SHA-256"
fi

BADGING=$("$BT/aapt2" dump badging "$APK")
echo "$BADGING" | grep -E "^package:|^sdkVersion|^targetSdkVersion|^application-label:|^native-code|^uses-feature(-not-required)?:" | sed 's/^/  /'
echo "$BADGING" | grep -q "^package: name='$PREVIEW_APP_ID'" && ok "applicationId $PREVIEW_APP_ID" || bad "applicationId"
echo "$BADGING" | grep -q "^application-label:'Mixxx Preview (unofficial)'" && ok "label" || bad "label"
echo "$BADGING" | grep -q "application-debuggable" && bad "debuggable" || ok "not debuggable"
echo "$BADGING" | grep -q "^sdkVersion:'31'" && ok "minSdk 31" || bad "minSdk"
echo "$BADGING" | grep -q "^native-code: 'arm64-v8a'$" && ok "arm64-v8a only" || bad "native-code"
echo "$BADGING" | grep -E "^package:" | grep -o "versionCode='[0-9]*' versionName='[^']*'"

chk "zipalign -c -P 16" "$BT/zipalign" -c -P 16 -v 4 "$APK"

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
unzip -q -o "$APK" 'lib/arm64-v8a/*.so' -d "$T"
n=0; badn=0
for so in "$T"/lib/arm64-v8a/*.so; do
  n=$((n+1))
  al=$("$LLVM/llvm-readelf" -lW "$so" | awk '$1=="LOAD"{print $NF}' | sort -u | tr '\n' ' ')
  [ "$al" = "0x4000 " ] || { badn=$((badn+1)); echo "  not 16 KB: $(basename "$so") $al"; }
done
[ "$badn" -eq 0 ] && ok "16 KB LOAD alignment: $n/$n libraries" || bad "16 KB LOAD alignment: $badn of $n libraries"

MX="$T/lib/arm64-v8a/libmixxx_arm64-v8a.so"
for sym in Java_dev_celtera_libremidi_DeviceObserver_onDeviceAddedNative Java_dev_celtera_libremidi_DeviceObserver_onDeviceRemovedNative \
           Java_dev_celtera_libremidi_MidiDeviceCallback_onDeviceOpenFailed Java_dev_celtera_libremidi_MidiDeviceCallback_onDeviceOpened; do
  "$LLVM/llvm-nm" -D "$MX" | grep -q " T $sym\$" && ok "JNI export $sym" || bad "JNI export $sym"
done
"$LLVM/llvm-nm" -D "$MX" | grep -q "Java_org_mixxx_MainActivity" && ok "MainActivity JNI export(s) present" || echo "NOTE no Java_org_mixxx_MainActivity_* export (check manually if MainActivity declares natives)"

DEX=$(mktemp -d); unzip -q -o "$APK" 'classes*.dex' -d "$DEX"
for c in 'Ldev/celtera/libremidi/DeviceObserver;' 'Ldev/celtera/libremidi/MidiDeviceCallback;' 'Lorg/mixxx/MainActivity;'; do
  found=0; for d in "$DEX"/classes*.dex; do "$BT/dexdump" "$d" | grep -q "Class descriptor *: *'$c'" && found=1; done
  [ $found = 1 ] && ok "dex class $c" || bad "dex class $c"
done
rm -rf "$DEX"

LIST=$(unzip -l "$APK")
for f in Hercules_DJControl_Inpulse_T7.midi.xml Hercules-DJControl-Inpulse-T7-script.js Hercules-DJControl-Inpulse-T7-platter.js midi-components-0.0.js; do
  echo "$LIST" | grep -q "assets/controllers/$f\$" && ok "asset controllers/$f" || bad "asset controllers/$f"
done
exit $fail

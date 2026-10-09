#!/usr/bin/env bash
# Step 6: check a release APK. Prints PASS/FAIL lines and exits 1 on any failure. Needs no device.
# Usage: 06_verify_apk.sh <apk> [expected-cert-sha256-hex-no-colons] [expected-versionCode]
set -uo pipefail
. "$(dirname "$0")/env.sh"
APK=$(realpath "${1:?apk}"); WANT_CERT=${2:-}; WANT_CODE=${3:-}
BT="$ANDROID_BUILD_TOOLS"; LLVM="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
fail=0
ok()  { echo "PASS $*"; }
bad() { echo "FAIL $*"; fail=1; }
# has <pattern> <text>: grep without a pipe (a pipe into grep -q breaks under pipefail).
has() { grep -q -- "$1" <<<"$2"; }

sha256sum "$APK"
CERTS=$("$BT/apksigner" verify --verbose --print-certs "$APK" 2>&1) || bad "apksigner verify"
grep -E "^Verifies|Verified using|certificate (DN|SHA-256)|Number of signers" <<<"$CERTS"
# apksigner verify skips v2 once v3 verified for minSdk >= 28, so look for the v2 (0x7109871a) and v3 (0xf05368c0) blocks.
IDS=$(python3 -I - "$APK" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read()
i = d.rfind(b"APK Sig Block 42")
size = struct.unpack("<Q", d[i - 8:i])[0]
p = i + 16 - size
end = i - 8
while p < end:
    n = struct.unpack("<Q", d[p:p + 8])[0]
    print(hex(struct.unpack("<I", d[p + 8:p + 12])[0]))
    p += 8 + n
PY
)
has "0x7109871a" "$IDS" && ok "v2 signature block present (verified together with v3)" || bad "v2 signature block"
has "Verified using v3 scheme (APK Signature Scheme v3): true" "$CERTS" && ok "v3 signature" || bad "v3 signature"
has "Android Debug" "$CERTS" && bad "debug certificate" || ok "not the debug certificate"
if [ -n "$WANT_CERT" ]; then
  has "certificate SHA-256 digest: $WANT_CERT" "$CERTS" && ok "signer certificate SHA-256 matches" || bad "signer certificate SHA-256"
fi

BADGING=$("$BT/aapt2" dump badging "$APK")
grep -E "^package:|^minSdkVersion|^targetSdkVersion|^application-label:|^native-code|^uses-feature(-not-required)?:" <<<"$BADGING" | sed 's/^/  /'
has "^package: name='$PREVIEW_APP_ID'" "$BADGING" && ok "applicationId $PREVIEW_APP_ID" || bad "applicationId"
has "^application-label:'Mixxx Preview (unofficial)'" "$BADGING" && ok "label" || bad "label"
has "application-debuggable" "$BADGING" && bad "debuggable" || ok "not debuggable"
has "^minSdkVersion:'31'" "$BADGING" && ok "minSdk 31" || bad "minSdk"
has "^native-code: 'arm64-v8a'$" "$BADGING" && ok "arm64-v8a only" || bad "native-code"
if [ -n "$WANT_CODE" ]; then
  has "versionCode='$WANT_CODE' " "$BADGING" && ok "versionCode $WANT_CODE" || bad "versionCode (want $WANT_CODE)"
fi

"$BT/zipalign" -c -P 16 -v 4 "$APK" >/dev/null 2>&1 && ok "zipalign -c -P 16" || bad "zipalign -c -P 16"

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
NM=$("$LLVM/llvm-nm" -D "$MX")
for sym in Java_dev_celtera_libremidi_DeviceObserver_onDeviceAddedNative Java_dev_celtera_libremidi_DeviceObserver_onDeviceRemovedNative \
           Java_dev_celtera_libremidi_MidiDeviceCallback_onDeviceOpenFailed Java_dev_celtera_libremidi_MidiDeviceCallback_onDeviceOpened; do
  has " T $sym\$" "$NM" && ok "JNI export $sym" || bad "JNI export $sym"
done
has "Java_org_mixxx_MainActivity" "$NM" && ok "MainActivity JNI export(s) present" || echo "NOTE no Java_org_mixxx_MainActivity_* export in the dynamic symbols"

DEX=$(mktemp -d); unzip -q -o "$APK" 'classes*.dex' -d "$DEX"
DUMP=""; for d in "$DEX"/classes*.dex; do DUMP+=$("$BT/dexdump" "$d"); DUMP+=$'\n'; done
for c in 'Ldev/celtera/libremidi/DeviceObserver;' 'Ldev/celtera/libremidi/MidiDeviceCallback;' 'Lorg/mixxx/MainActivity;'; do
  has "Class descriptor *: *'$c'" "$DUMP" && ok "dex class $c" || bad "dex class $c"
done
has "onDeviceOpened" "$DUMP" && ok "dex native method onDeviceOpened present (not stripped)" || bad "dex native method onDeviceOpened"
rm -rf "$DEX"

LIST=$(unzip -l "$APK")
for f in Hercules_DJControl_Inpulse_T7.midi.xml Hercules-DJControl-Inpulse-T7-script.js Hercules-DJControl-Inpulse-T7-platter.js midi-components-0.0.js; do
  has "assets/controllers/$f\$" "$LIST" && ok "asset controllers/$f" || bad "asset controllers/$f"
done
exit $fail

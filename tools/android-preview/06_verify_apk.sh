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

# No Fraunhofer FDK-AAC library code anywhere in the shipped native code (step 3b removes it from FFmpeg; step 4 drops
# Qt's own FFmpeg media plugin, which embeds it), and FFmpeg keeps its native LGPL AAC decoder and encoder.
# Mixxx's own EncoderFdkAac only dlopen()s a user-installed libfdk-aac, so class names and messages mentioning FDK in
# libmixxx are expected (including the function-name strings it passes to dlsym); what must be absent are the
# library's symbols and its identifying strings.
FDKSYM='ff_libfdk_aac|aacEncOpen|aacEncEncode|aacDecoder_Open|aacDecoder_DecodeFrame|FDKmem|FDKcalloc|FDK_[A-Za-z]'
FDKSTR='Fraunhofer|FDK AAC|FDK-AAC codec|FDKmem|ff_libfdk_aac|enable-libfdk'
fdk_hits=0
for so in "$T"/lib/arm64-v8a/*.so; do
  H=$({ "$LLVM/llvm-nm" "$so" 2>/dev/null; "$LLVM/llvm-nm" -D "$so" 2>/dev/null; } | grep -Ec "$FDKSYM")
  S=$(strings -n 6 "$so" | grep -Ec "$FDKSTR")
  if [ "$H" -ne 0 ] || [ "$S" -ne 0 ]; then fdk_hits=$((fdk_hits+1)); echo "  FDK evidence in $(basename "$so"): symbols=$H strings=$S"; fi
done
[ "$fdk_hits" -eq 0 ] && ok "no FDK-AAC library symbols or strings in any shipped .so" || bad "FDK-AAC evidence in $fdk_hits libraries"
has "libplugins_multimedia_ffmpegmediaplugin" "$(unzip -l "$APK")" && bad "Qt ffmpegmediaplugin (embeds FDK) is in the APK" || ok "Qt ffmpegmediaplugin not bundled"
CFG=$(strings -n 20 "$MX" | grep -m1 -e "--enable-pic" || true)
has "enable-libfdk" "$CFG" && bad "FFmpeg configuration string enables libfdk" || ok "FFmpeg configuration string: libfdk not enabled"
has "disable-libfdk-aac" "$CFG" && ok "FFmpeg configuration string has --disable-libfdk-aac" || bad "FFmpeg configuration string missing --disable-libfdk-aac"
has "enable-decoder=.*,aac,.*aac_latm" "$CFG" && ok "FFmpeg configuration enables native decoders aac, aac_latm" || bad "FFmpeg configuration lacks native aac decoders"
has "AAC LATM (Advanced Audio Coding LATM syntax)" "$(strings -n 8 "$MX")" && ok "native AAC LATM decoder description present" || bad "native AAC LATM decoder missing"
# Exact codec symbols exist only in the unstripped library: UNSTRIPPED_MIXXX=<build>/libmixxx_arm64-v8a.so
if [ -n "${UNSTRIPPED_MIXXX:-}" ]; then
  US=$("$LLVM/llvm-nm" "$UNSTRIPPED_MIXXX" 2>/dev/null)
  for sym in ff_aac_decoder ff_aac_latm_decoder ff_aac_encoder ff_alac_decoder ff_mp3float_decoder ff_flac_decoder ff_mov_demuxer; do
    has " [DdRr] $sym\$" "$US" && ok "FFmpeg symbol $sym present (unstripped libmixxx)" || bad "FFmpeg symbol $sym missing (unstripped libmixxx)"
  done
  has "ff_libfdk_aac" "$US" && bad "ff_libfdk_aac in unstripped libmixxx" || ok "no ff_libfdk_aac in unstripped libmixxx"
fi

LIST=$(unzip -l "$APK")
for f in Hercules_DJControl_Inpulse_T7.midi.xml Hercules-DJControl-Inpulse-T7-script.js Hercules-DJControl-Inpulse-T7-platter.js midi-components-0.0.js; do
  has "assets/controllers/$f\$" "$LIST" && ok "asset controllers/$f" || bad "asset controllers/$f"
done
# --- Public-release hygiene (rc2 and later) ---------------------------------------------------------------------
# No build-host paths or user names in any shipped file (assert/__FILE__ strings, FFmpeg configure line, debug info).
# Checks the user running this script, the build/source/work directories, WSL/Windows mount paths and Windows drive paths.
ALLX=$(mktemp -d); unzip -q -o "$APK" -d "$ALLX"
PRIV="$HOME|/mnt/[a-z]/|[A-Za-z]:[/\\]+(Users|Documents|VectorSTEMS|t7-)|/Users/[A-Za-z0-9_.-]+/"
[ -n "${USER:-}" ] && PRIV="$PRIV|/home/$USER|\b$USER\b"
[ -n "${PREVIEW_WORK:-}" ] && PRIV="$PRIV|$PREVIEW_WORK"
[ -n "${PREVIEW_SRC:-}" ] && PRIV="$PRIV|$PREVIEW_SRC"
priv_hits=0
while IFS= read -r -d '' f; do
  if [ "${f##*.}" = so ]; then CNT=$(strings -n 6 "$f" | grep -Ec -- "$PRIV"); else CNT=$(grep -aEc -- "$PRIV" "$f"); fi
  if [ "$CNT" -ne 0 ]; then priv_hits=$((priv_hits+1)); echo "  private path/name evidence in ${f#$ALLX/}: $CNT lines"; fi
done < <(find "$ALLX" -type f -print0)
[ "$priv_hits" -eq 0 ] && ok "no private paths or user names in any file of the APK" || bad "private paths or user names in $priv_hits files"
LIST=$(unzip -l "$APK")
has "qmltooling" "$LIST" && bad "QML debugger/profiler plugins (qmltooling) are in the APK" || ok "no qmltooling (QML debugger/profiler) plugins"
for f in COPYING LICENSE GPL-3.0.txt NOTICE; do
  has "assets/legal/$f\$" "$LIST" && ok "asset legal/$f" || bad "asset legal/$f"
done
if [ -f "$PREVIEW_SRC/NOTICE" ]; then
  cmp -s "$ALLX/assets/legal/NOTICE" "$PREVIEW_SRC/NOTICE" && ok "assets/legal/NOTICE equals the repository NOTICE" || bad "assets/legal/NOTICE differs from the repository NOTICE"
fi
# NOTICE must not mention an open licence question or FDK other than saying it is excluded.
[ -f "$ALLX/assets/legal/NOTICE" ] && ! grep -qi "open question" "$ALLX/assets/legal/NOTICE" && ok "NOTICE has no open-question wording" || bad "NOTICE missing or has open-question wording"
# PREVIEW badge on the launcher icon: the adaptive icon uses the preview foreground, and the drawable is in the APK.
RES=$("$BT/aapt2" dump resources "$APK")
has "ic_launcher_preview_foreground" "$RES" && ok "launcher icon: PREVIEW badge drawable present" || bad "launcher icon: PREVIEW badge drawable missing"
ICONXML=$("$BT/aapt2" dump xmltree --file res/mipmap-anydpi-v26/ic_launcher.xml "$APK" 2>&1 || true)
has "foreground" "$ICONXML" && has "ic_launcher_preview_foreground\|0x7f" "$ICONXML" && ok "adaptive icon xml has a foreground" || bad "adaptive icon xml"
# Permissions: exactly the documented set.
PERMS=$("$BT/aapt2" dump permissions "$APK" | sed -n "s/^uses-permission: name='\(.*\)'.*/\1/p" | sort | tr '\n' ' ')
echo "  permissions: $PERMS"
WANTP="android.permission.ACCESS_NETWORK_STATE android.permission.INTERNET android.permission.MANAGE_EXTERNAL_STORAGE android.permission.MODIFY_AUDIO_SETTINGS android.permission.RECORD_AUDIO android.permission.USB_PERMISSION android.permission.WAKE_LOCK android.permission.WRITE_EXTERNAL_STORAGE $PREVIEW_APP_ID.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION "
[ "$PERMS" = "$WANTP" ] && ok "permissions are exactly the documented set" || bad "permissions differ from the documented set"
# Source state of the build (optional): BUILD_LOG=<log of 04_configure_build.sh> proves a clean committed tree.
if [ -n "${BUILD_LOG:-}" ]; then
  has "Git worktree modified: no" "$(cat "$BUILD_LOG")" && ok "build log: Git worktree modified: no" || bad "build log: worktree was modified or line missing"
  DESC=$(sed -n 's/.*Git describe: //p' "$BUILD_LOG" | head -1); echo "  Git describe: $DESC"
  has "-modified" "$DESC" && bad "git describe says -modified" || ok "git describe is not -modified"
  HEADS=$(git -C "$PREVIEW_SRC" rev-parse --short=10 HEAD 2>/dev/null || true)
  [ -n "$HEADS" ] && { has "g$HEADS" "$DESC" && ok "build is from HEAD $HEADS" || bad "build commit does not match HEAD $HEADS"; }
fi
rm -rf "$ALLX"
exit $fail

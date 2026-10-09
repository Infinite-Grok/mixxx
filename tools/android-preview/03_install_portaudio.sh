#!/usr/bin/env bash
# Step 3: put the repaired libportaudio.a into YOUR extracted copy of the dependency tree (lib/libportaudio.a
# only; the debug library and headers are untouched, RelWithDebInfo links the release library).
# The downloaded zip is never touched. Hashes before/after go to $PREVIEW_WORK/portaudio-integration.txt.
set -euo pipefail
. "$(dirname "$0")/env.sh"
NEW="$PREVIEW_WORK/portaudio/install/lib/libportaudio.a"
OUT="$PREVIEW_WORK/portaudio-integration.txt"
test -f "$NEW"
{
  echo "# before (archive original)"; sha256sum "$DEPS_PREFIX/lib/libportaudio.a"
  echo "# repaired"; sha256sum "$NEW"
} | tee "$OUT"
cp -- "$NEW" "$DEPS_PREFIX/lib/libportaudio.a"
{ echo "# after"; sha256sum "$DEPS_PREFIX/lib/libportaudio.a"; } | tee -a "$OUT"

#!/usr/bin/env bash
# Step 1: get the pinned Mixxx Android dependency archive and extract it into a tree you own.
# The zip is verified by sha256 and never modified. Needs ~3.6 GB for the zip and ~12 GB extracted.
set -euo pipefail
. "$(dirname "$0")/env.sh"
mkdir -p "$PREVIEW_WORK" "$DEPS_ROOT"
if [ ! -f "$BUILDENV_ZIP" ]; then
  echo "downloading $BUILDENV_URL"
  curl -L --fail -o "$BUILDENV_ZIP.part" "$BUILDENV_URL" && mv "$BUILDENV_ZIP.part" "$BUILDENV_ZIP"
fi
echo "$BUILDENV_SHA256  $BUILDENV_ZIP" | sha256sum -c -
[ -e "$DEPS_DIR" ] && { echo "$DEPS_DIR exists; remove it to re-extract" >&2; exit 1; }
unzip -q "$BUILDENV_ZIP" -d "$DEPS_ROOT"
test -f "$DEPS_PREFIX/lib/libportaudio.a"
echo "extracted: $DEPS_DIR"
sha256sum "$DEPS_PREFIX/lib/libportaudio.a" | tee "$PREVIEW_WORK/libportaudio.original.sha256"

#!/bin/bash
# make-icns.sh <source-1024px.png> <output.icns>
# Builds a .icns from a single 1024px PNG using sips + iconutil.
set -euo pipefail

SRC="$1"
OUT="$2"
TMP="$(mktemp -d)/AppIcon.iconset"
mkdir -p "$TMP"

for size in 16 32 128 256 512; do
    sips -z $size $size "$SRC" --out "$TMP/icon_${size}x${size}.png" >/dev/null
    double=$((size * 2))
    sips -z $double $double "$SRC" --out "$TMP/icon_${size}x${size}@2x.png" >/dev/null
done

iconutil -c icns "$TMP" -o "$OUT"
rm -rf "$(dirname "$TMP")"

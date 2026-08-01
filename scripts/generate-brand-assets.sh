#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SOURCE="$ROOT_DIR/apps/launcher/assets/oos-logo.svg"
OUTPUT="$ROOT_DIR/apps/launcher/assets/oos-logo-32-bgra.bin"
CHROME_BIN=${CHROME_BIN:-google-chrome}

if ! command -v magick >/dev/null 2>&1; then
  echo "ImageMagick is required to generate OOS brand assets." >&2
  exit 1
fi
if ! command -v "$CHROME_BIN" >/dev/null 2>&1; then
  echo "Chrome is required to render OOS brand assets." >&2
  exit 1
fi

temporary=$(mktemp "${TMPDIR:-/tmp}/oos-logo.XXXXXX.png")
trap 'rm -f "$temporary"' EXIT
"$CHROME_BIN" --headless --no-sandbox --disable-gpu --hide-scrollbars \
  --force-device-scale-factor=1 --default-background-color=00000000 \
  --window-size=1024,1024 --screenshot="$temporary" "file://$SOURCE"

magick "$temporary" -trim -resize 30x30 -background none -gravity center \
  -extent 32x32 -alpha on -depth 8 "bgra:$OUTPUT"

size=$(stat -c %s "$OUTPUT")
if [[ "$size" -ne 4096 ]]; then
  echo "Generated logo image has invalid size: $size" >&2
  exit 1
fi

echo "Generated $OUTPUT"
echo "Rebuild OOS to embed the updated brand assets"

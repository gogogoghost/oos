#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DEVICE=${1:-nokia-2780-flip}
SOURCE="$ROOT_DIR/system/assets/boot/$DEVICE/boot-splash.svg"
OUTPUT="$ROOT_DIR/system/assets/boot/$DEVICE/boot-splash.png"
CHROME_BIN=${CHROME_BIN:-google-chrome}

if [[ ! -f "$SOURCE" ]]; then
  echo "Missing boot splash source: $SOURCE" >&2
  exit 1
fi
if ! command -v "$CHROME_BIN" >/dev/null 2>&1; then
  echo "Chrome is required to render the boot splash SVG." >&2
  exit 1
fi

temporary=$(mktemp "${TMPDIR:-/tmp}/oos-boot-splash.XXXXXX.png")
trap 'rm -f "$temporary"' EXIT
"$CHROME_BIN" --headless --no-sandbox --disable-gpu --hide-scrollbars \
  --force-device-scale-factor=1 --window-size=240,320 \
  --screenshot="$temporary" "file://$SOURCE"

dimensions=$(identify -format '%wx%h' "$temporary")
if [[ "$dimensions" != 240x320 ]]; then
  echo "Rendered boot splash has invalid dimensions: $dimensions" >&2
  exit 1
fi
mv "$temporary" "$OUTPUT"
trap - EXIT
echo "Generated $OUTPUT"
echo "Rebuild OOS to embed the updated boot splash"

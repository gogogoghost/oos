#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUTPUT_DIR="$ROOT_DIR/build/native-apps"
SOLID_DIR="$ROOT_DIR/apps/tests/solid-demo"
mkdir -p "$OUTPUT_DIR"

"$ROOT_DIR/scripts/fetch-ui-frameworks.sh"
"$ROOT_DIR/scripts/build-c-lvgl-demo.sh" "$OUTPUT_DIR/lvgl-demo.wasm"
"$ROOT_DIR/scripts/build-c-app.sh" "$OUTPUT_DIR/clay-demo.wasm" \
  "$ROOT_DIR/apps/tests/clay-demo/main.c"

npm ci --prefix "$SOLID_DIR"
npm run check --prefix "$SOLID_DIR"
npm run build --prefix "$SOLID_DIR"

"$ROOT_DIR/scripts/package-oos-app.sh" \
  --manifest "$ROOT_DIR/apps/tests/lvgl-demo/manifest.json" \
  --wasm "$OUTPUT_DIR/lvgl-demo.wasm" \
  --output "$OUTPUT_DIR/lvgl-demo.zip"
"$ROOT_DIR/scripts/package-oos-app.sh" \
  --manifest "$ROOT_DIR/apps/tests/clay-demo/manifest.json" \
  --wasm "$OUTPUT_DIR/clay-demo.wasm" \
  --output "$OUTPUT_DIR/clay-demo.zip"
"$ROOT_DIR/scripts/package-oos-app.sh" \
  --manifest "$SOLID_DIR/manifest.json" \
  --js "$OUTPUT_DIR/solid-demo/main.mjs" \
  --output "$OUTPUT_DIR/solid-demo.zip"

echo "Built and packaged LVGL, Clay, and Solid framework demos"

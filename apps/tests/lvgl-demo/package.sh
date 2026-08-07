#!/usr/bin/env bash

set -euo pipefail

APP_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$APP_DIR/../../.." && pwd)
BUILD_DIR="$APP_DIR/build"
DIST_DIR="$APP_DIR/dist"
WASM="$BUILD_DIR/main.wasm"

"$ROOT_DIR/scripts/fetch-ui-frameworks.sh"
mapfile -d '' LVGL_SOURCES < <(
  find "$ROOT_DIR/third_party/lvgl/src" -type f -name '*.c' -print0 | sort -z
)
mkdir -p "$BUILD_DIR" "$DIST_DIR"
OOS_LV_CONF_DIR="$APP_DIR" \
  "$ROOT_DIR/scripts/build-c-app.sh" "$WASM" \
  "$ROOT_DIR/sdk/c/lvgl/oos_lvgl_backend.c" \
  "$APP_DIR/main.c" "${LVGL_SOURCES[@]}"
"$ROOT_DIR/scripts/package-oos-app.sh" \
  --manifest "$APP_DIR/manifest.json" \
  --wasm "$WASM" \
  --output "$DIST_DIR/application.zip"

echo "Packaged LVGL demo at $DIST_DIR/application.zip"

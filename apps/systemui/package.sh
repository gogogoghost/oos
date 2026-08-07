#!/usr/bin/env bash

set -euo pipefail

APP_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$APP_DIR/../.." && pwd)
BUILD_DIR="$APP_DIR/build"
DIST_DIR="$APP_DIR/dist"
WASM="$BUILD_DIR/main.wasm"

"$ROOT_DIR/scripts/fetch-ui-frameworks.sh"
mapfile -d '' LVGL_SOURCES < <(
  find "$ROOT_DIR/third_party/lvgl/src" -type f -name '*.c' -print0 | sort -z
)
mkdir -p "$BUILD_DIR" "$DIST_DIR"
OOS_LV_CONF_DIR="$APP_DIR" OOS_GUEST_INCLUDE_DIRS="$APP_DIR/include" \
  "$ROOT_DIR/scripts/build-cpp-app.sh" "$WASM" \
  "$ROOT_DIR/sdk/cpp/guest/src/platform.cpp" \
  "$ROOT_DIR/sdk/cpp/guest/src/system_ui_settings.cpp" \
  "$ROOT_DIR/sdk/cpp/guest/src/fonts.cpp" \
  "$ROOT_DIR/sdk/cpp/src/lvgl_backend.cpp" \
  "$APP_DIR/src/system_ui.cpp" \
  "$APP_DIR/src/wasm_main.cpp" "${LVGL_SOURCES[@]}"
"$ROOT_DIR/scripts/package-oos-app.sh" \
  --manifest "$APP_DIR/manifest.json" \
  --wasm "$WASM" \
  --output "$DIST_DIR/application.zip"

echo "Packaged SystemUI at $DIST_DIR/application.zip"

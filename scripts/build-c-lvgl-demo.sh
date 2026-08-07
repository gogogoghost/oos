#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DEMO_DIR="$ROOT_DIR/apps/tests/lvgl-demo"
OUTPUT=${1:-"$ROOT_DIR/build/native-apps/lvgl-demo.wasm"}

"$ROOT_DIR/scripts/fetch-ui-frameworks.sh"
mapfile -d '' LVGL_SOURCES < <(
  find "$ROOT_DIR/third_party/lvgl/src" -type f -name '*.c' -print0 | sort -z
)
OOS_LV_CONF_DIR="$DEMO_DIR" \
  "$ROOT_DIR/scripts/build-c-app.sh" "$OUTPUT" \
  "$ROOT_DIR/sdk/c/lvgl/oos_lvgl_backend.c" \
  "$DEMO_DIR/main.c" "${LVGL_SOURCES[@]}"

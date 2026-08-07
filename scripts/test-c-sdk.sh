#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build/c-sdk-test"
mkdir -p "$BUILD_DIR"
"$ROOT_DIR/scripts/generate-c-sdk.sh" --check
"$ROOT_DIR/scripts/fetch-ui-frameworks.sh"
"$ROOT_DIR/scripts/build-c-production-libc-smoke.sh"
clang -std=c17 -O2 -Werror -Wno-unknown-attributes \
  -I"$ROOT_DIR/sdk/c/generated" \
  -c "$ROOT_DIR/sdk/c/generated/app.c" -o "$BUILD_DIR/app.o"
clang -std=c17 -O2 -Werror -Wno-unknown-attributes \
  -I"$ROOT_DIR/sdk/c/generated" \
  -c "$ROOT_DIR/sdk/c/tests/media_api_compile_test.c" \
  -o "$BUILD_DIR/media_api_compile_test.o"
clang -std=c17 -O2 -Werror -Wno-unknown-attributes \
  -DLV_CONF_INCLUDE_SIMPLE \
  -I"$ROOT_DIR/sdk/c/generated" -I"$ROOT_DIR/sdk/c/lvgl" \
  -I"$ROOT_DIR/third_party/lvgl" -I"$ROOT_DIR/system/config/lvgl" \
  -c "$ROOT_DIR/sdk/c/lvgl/oos_lvgl_backend.c" \
  -o "$BUILD_DIR/oos_lvgl_backend.o"
echo "C SDK generated bindings and media API compile test passed"

#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUTPUT_DIR="$ROOT_DIR/build/native-apps"
MODULE_DIR="$OUTPUT_DIR/package-modules"
mkdir -p "$MODULE_DIR"

"$ROOT_DIR/scripts/build-c-module.sh" \
  "$MODULE_DIR/echo.wasm" \
  "$ROOT_DIR/sdk/c/tests/module_echo.c"
cp "$ROOT_DIR/sdk/c/tests/module_echo.mjs" "$MODULE_DIR/js-echo.mjs"
"$ROOT_DIR/scripts/build-c-app.sh" \
  "$OUTPUT_DIR/c-module-parent.wasm" \
  "$ROOT_DIR/sdk/c/tests/module_parent.c"

echo "Built C package module smoke artifacts"

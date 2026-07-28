#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build/host-wamr-runtime"

"$ROOT_DIR/scripts/fetch-wamr.sh"
"$ROOT_DIR/scripts/build-native-apps.sh"
"$ROOT_DIR/scripts/verify-wit-interfaces.sh"
cmake -S "$ROOT_DIR/system/tests/host" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR"
"$BUILD_DIR/oos_wasm_runtime_test" \
  "$ROOT_DIR/build/native-apps/launcher.wasm" \
  "$ROOT_DIR/build/native-apps/wit-smoke.wasm"

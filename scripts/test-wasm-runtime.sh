#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build/host-wamr-runtime"

"$ROOT_DIR/scripts/fetch-wamr.sh"
"$ROOT_DIR/scripts/build-media-codecs.sh" local
"$ROOT_DIR/scripts/build-native-apps.sh"
"$ROOT_DIR/scripts/test-c-sdk.sh"
"$ROOT_DIR/scripts/build-c-thread-smoke.sh"
"$ROOT_DIR/scripts/build-c-worker-wit-trap.sh"
"$ROOT_DIR/scripts/build-c-exit-smoke.sh"
"$ROOT_DIR/scripts/build-c-subruntime-smoke.sh"
"$ROOT_DIR/scripts/build-c-thread-smoke-aot.sh"
"$ROOT_DIR/scripts/build-c-subruntime-smoke-aot.sh"
if command -v wasm-tools >/dev/null 2>&1 &&
    [[ -f "$ROOT_DIR/build/native-apps/egui-demo.component.wasm" ]]; then
  "$ROOT_DIR/scripts/verify-wit-interfaces.sh"
else
  echo "Skipping Component Model verification: wasm-tools/component output unavailable"
fi
cmake -S "$ROOT_DIR/system/tests/host" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR"
"$BUILD_DIR/oos_wasm_runtime_test" \
  "$ROOT_DIR/build/native-apps/egui-demo.wasm" \
  "$ROOT_DIR/build/native-apps/wit-smoke.wasm" \
  "$ROOT_DIR/build/native-apps/c-thread-smoke.wasm" \
  "$ROOT_DIR/build/native-apps/c-worker-wit-trap.wasm" \
  "$ROOT_DIR/build/native-apps/c-exit-smoke.wasm" \
  "$ROOT_DIR/system/assets/fonts" \
  "$ROOT_DIR/build/native-apps/c-subruntime-parent.wasm" \
  "$ROOT_DIR/build/native-apps/subruntime-host-modules" \
  "$ROOT_DIR/build/native-apps/c-production-libc-smoke.wasm"

TEST_DIRECTORY=$(mktemp -d "${TMPDIR:-/tmp}/oos-test-application.XXXXXX")
TEST_AOT_PACKAGE="$TEST_DIRECTORY/aot-only.zip"
TEST_WASM_PACKAGE="$TEST_DIRECTORY/wasm-only.zip"
trap 'rm -rf "$TEST_DIRECTORY"' EXIT
"$ROOT_DIR/system/tests/host/create-app-package.sh" "$TEST_AOT_PACKAGE" aot
"$BUILD_DIR/oos_app_repository_test" "$TEST_AOT_PACKAGE" entry.aot
"$ROOT_DIR/system/tests/host/create-app-package.sh" "$TEST_WASM_PACKAGE" wasm
"$BUILD_DIR/oos_app_repository_test" "$TEST_WASM_PACKAGE" entry.wasm
"$BUILD_DIR/oos_device_storage_test"
"$BUILD_DIR/oos_font_assets_test"
"$BUILD_DIR/oos_system_service_test"
LD_LIBRARY_PATH="$BUILD_DIR/third_party/fluidlite:$ROOT_DIR/build/media-codecs/local/install/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$BUILD_DIR/oos_media_core_test"

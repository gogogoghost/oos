#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build/host-wamr-runtime"

"$ROOT_DIR/scripts/fetch-wamr.sh"
"$ROOT_DIR/scripts/apply-wamr-web-patches.sh"
"$ROOT_DIR/scripts/build-native-apps.sh"
if command -v wasm-tools >/dev/null 2>&1 &&
    [[ -f "$ROOT_DIR/build/native-apps/launcher.component.wasm" ]]; then
  "$ROOT_DIR/scripts/verify-wit-interfaces.sh"
else
  echo "Skipping Component Model verification: wasm-tools/component output unavailable"
fi
cmake -S "$ROOT_DIR/system/tests/host" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR"
"$BUILD_DIR/oos_wamr_c_api_lifetime_test"
"$BUILD_DIR/oos_wasm_runtime_test" \
  "$ROOT_DIR/build/native-apps/launcher.wasm" \
  "$ROOT_DIR/build/native-apps/wit-smoke.wasm" \
  "$ROOT_DIR/system/assets/fonts"

TEST_DIRECTORY=$(mktemp -d "${TMPDIR:-/tmp}/oos-test-application.XXXXXX")
TEST_PACKAGE="$TEST_DIRECTORY/application.zip"
KAIOS25_PACKAGE="$TEST_DIRECTORY/kaios25.zip"
KAIOS3_PACKAGE="$TEST_DIRECTORY/kaios3.zip"
trap 'rm -rf "$TEST_DIRECTORY"' EXIT
"$ROOT_DIR/system/tests/host/create-app-package.sh" "$TEST_PACKAGE"
"$ROOT_DIR/system/tests/host/create-kaios-packages.sh" \
  "$KAIOS25_PACKAGE" "$KAIOS3_PACKAGE"
"$BUILD_DIR/oos_app_repository_test" "$TEST_PACKAGE" \
  "$KAIOS25_PACKAGE" "$KAIOS3_PACKAGE"
"$BUILD_DIR/oos_device_storage_test"
"$BUILD_DIR/oos_font_assets_test"
"$BUILD_DIR/oos_system_service_test"

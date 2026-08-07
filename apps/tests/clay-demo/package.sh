#!/usr/bin/env bash

set -euo pipefail

APP_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$APP_DIR/../../.." && pwd)
BUILD_DIR="$APP_DIR/build"
DIST_DIR="$APP_DIR/dist"
WASM="$BUILD_DIR/main.wasm"

"$ROOT_DIR/scripts/fetch-ui-frameworks.sh"
mkdir -p "$BUILD_DIR" "$DIST_DIR"
"$ROOT_DIR/scripts/build-c-app.sh" "$WASM" "$APP_DIR/main.c"
"$ROOT_DIR/scripts/package-oos-app.sh" \
  --manifest "$APP_DIR/manifest.json" \
  --wasm "$WASM" \
  --output "$DIST_DIR/application.zip"

echo "Packaged Clay demo at $DIST_DIR/application.zip"

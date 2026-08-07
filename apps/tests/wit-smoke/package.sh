#!/usr/bin/env bash

set -euo pipefail

APP_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$APP_DIR/../../.." && pwd)
TARGET=wasm32-unknown-unknown
BUILD_DIR="$APP_DIR/build"
DIST_DIR="$APP_DIR/dist"
SOURCE="$APP_DIR/target/$TARGET/release/oos_wit_smoke.wasm"
WASM="$BUILD_DIR/main.wasm"

export RUSTFLAGS="${RUSTFLAGS:-} -C link-arg=--max-memory=67108864"
cargo build --manifest-path "$APP_DIR/Cargo.toml" --target "$TARGET" --release
mkdir -p "$BUILD_DIR" "$DIST_DIR"
if ! cmp -s "$SOURCE" "$WASM"; then
  install -m 0644 "$SOURCE" "$WASM"
fi
"$ROOT_DIR/scripts/package-oos-app.sh" \
  --manifest "$APP_DIR/manifest.json" \
  --wasm "$WASM" \
  --output "$DIST_DIR/application.zip"

echo "Packaged WIT smoke app at $DIST_DIR/application.zip"

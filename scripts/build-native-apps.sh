#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TARGET=wasm32-unknown-unknown
APPS_TARGET_DIR="$ROOT_DIR/build/cargo"

if ! command -v wasm-tools >/dev/null 2>&1; then
  echo "wasm-tools is required to build WIT component artifacts." >&2
  exit 1
fi

CARGO_TARGET_DIR="$APPS_TARGET_DIR" cargo build \
  --manifest-path "$ROOT_DIR/apps/Cargo.toml" \
  --target "$TARGET" \
  --release \
  -p oos-launcher

CARGO_TARGET_DIR="$APPS_TARGET_DIR" cargo build \
  --manifest-path "$ROOT_DIR/apps/Cargo.toml" \
  --target "$TARGET" \
  --release \
  -p oos-wit-smoke

mkdir -p "$ROOT_DIR/build/native-apps"
SOURCE="$APPS_TARGET_DIR/$TARGET/release/oos_launcher.wasm"
DESTINATION="$ROOT_DIR/build/native-apps/launcher.wasm"
if ! cmp -s "$SOURCE" "$DESTINATION"; then
  cp "$SOURCE" "$DESTINATION"
fi

SMOKE_SOURCE="$APPS_TARGET_DIR/$TARGET/release/oos_wit_smoke.wasm"
SMOKE_DESTINATION="$ROOT_DIR/build/native-apps/wit-smoke.wasm"
if ! cmp -s "$SMOKE_SOURCE" "$SMOKE_DESTINATION"; then
  cp "$SMOKE_SOURCE" "$SMOKE_DESTINATION"
fi

COMPONENT_DESTINATION="$ROOT_DIR/build/native-apps/launcher.component.wasm"
wasm-tools component new "$DESTINATION" -o "$COMPONENT_DESTINATION"
wasm-tools validate "$COMPONENT_DESTINATION"

echo "Built $DESTINATION, $COMPONENT_DESTINATION, and $SMOKE_DESTINATION"

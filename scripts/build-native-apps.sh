#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TARGET=wasm32-unknown-unknown
DEMO_DIR="$ROOT_DIR/apps/tests/egui-demo"
SMOKE_DIR="$ROOT_DIR/apps/tests/wit-smoke"
APPS_TARGET_DIR="$ROOT_DIR/build/cargo"

CARGO_TARGET_DIR="$APPS_TARGET_DIR" cargo build \
  --manifest-path "$DEMO_DIR/Cargo.toml" \
  --target "$TARGET" \
  --release

CARGO_TARGET_DIR="$APPS_TARGET_DIR" cargo build \
  --manifest-path "$SMOKE_DIR/Cargo.toml" \
  --target "$TARGET" \
  --release

mkdir -p "$ROOT_DIR/build/native-apps"
SOURCE="$APPS_TARGET_DIR/$TARGET/release/oos_egui_demo.wasm"
DESTINATION="$ROOT_DIR/build/native-apps/egui-demo.wasm"
if ! cmp -s "$SOURCE" "$DESTINATION"; then
  cp "$SOURCE" "$DESTINATION"
fi

SMOKE_SOURCE="$APPS_TARGET_DIR/$TARGET/release/oos_wit_smoke.wasm"
SMOKE_DESTINATION="$ROOT_DIR/build/native-apps/wit-smoke.wasm"
if ! cmp -s "$SMOKE_SOURCE" "$SMOKE_DESTINATION"; then
  cp "$SMOKE_SOURCE" "$SMOKE_DESTINATION"
fi

COMPONENT_DESTINATION="$ROOT_DIR/build/native-apps/egui-demo.component.wasm"
if command -v wasm-tools >/dev/null 2>&1; then
  wasm-tools component new "$DESTINATION" -o "$COMPONENT_DESTINATION"
  wasm-tools validate "$COMPONENT_DESTINATION"
  echo "Built $DESTINATION, $COMPONENT_DESTINATION, and $SMOKE_DESTINATION"
else
  echo "Built $DESTINATION and $SMOKE_DESTINATION"
  echo "wasm-tools is unavailable; skipped the optional Component Model artifact" >&2
fi

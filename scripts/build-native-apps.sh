#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TARGET=wasm32-unknown-unknown

cargo build \
  --manifest-path "$ROOT_DIR/Cargo.toml" \
  --target "$TARGET" \
  --release \
  -p oos-launcher

mkdir -p "$ROOT_DIR/build/native-apps"
SOURCE="$ROOT_DIR/target/$TARGET/release/oos_launcher.wasm"
DESTINATION="$ROOT_DIR/build/native-apps/launcher.wasm"
if ! cmp -s "$SOURCE" "$DESTINATION"; then
  cp "$SOURCE" "$DESTINATION"
fi

echo "Built $DESTINATION"

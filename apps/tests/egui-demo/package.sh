#!/usr/bin/env bash

set -euo pipefail

APP_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$APP_DIR/../../.." && pwd)
TARGET=wasm32-unknown-unknown
BUILD_DIR="$APP_DIR/build"
DIST_DIR="$APP_DIR/dist"
SOURCE="$APP_DIR/target/$TARGET/release/oos_egui_demo.wasm"
WASM="$BUILD_DIR/main.wasm"
COMPONENT="$BUILD_DIR/main.component.wasm"

export RUSTFLAGS="${RUSTFLAGS:-} -C link-arg=--max-memory=67108864"
cargo build --manifest-path "$APP_DIR/Cargo.toml" --target "$TARGET" --release
mkdir -p "$BUILD_DIR" "$DIST_DIR"
if ! cmp -s "$SOURCE" "$WASM"; then
  install -m 0644 "$SOURCE" "$WASM"
fi

if command -v wasm-tools >/dev/null 2>&1; then
  wasm-tools component new "$WASM" -o "$COMPONENT"
  wasm-tools validate "$COMPONENT"
else
  echo "wasm-tools is unavailable; skipped optional Component Model output" >&2
fi

AOT_ARGS=()
shopt -s nullglob
for artifact in "$BUILD_DIR"/main.*.aot; do
  if [[ "$artifact" -ot "$WASM" ]]; then
    echo "Skipping stale AOT artifact: $artifact" >&2
    continue
  fi
  target=${artifact%.aot}
  target=${target##*.}
  AOT_ARGS+=(--aot "$target=$artifact")
done
shopt -u nullglob

"$ROOT_DIR/scripts/package-oos-app.sh" \
  --manifest "$APP_DIR/manifest.json" \
  --wasm "$WASM" \
  "${AOT_ARGS[@]}" \
  --output "$DIST_DIR/application.zip"

echo "Packaged egui demo at $DIST_DIR/application.zip"

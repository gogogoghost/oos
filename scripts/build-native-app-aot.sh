#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$ROOT_DIR/scripts/build-native-apps.sh"
"$ROOT_DIR/scripts/fetch-wamr.sh"

if [[ -f "$ROOT_DIR/.env" ]]; then
  set -a
  source "$ROOT_DIR/.env"
  set +a
fi

WAMRC="$ROOT_DIR/build/host-wamrc/wamrc"
INPUT="$ROOT_DIR/build/native-apps/launcher.wasm"
OUTPUT="$ROOT_DIR/build/native-apps/launcher.aot"
BUILD_CONTAINER=${OOS_WAMR_DISTROBOX:-}

if [[ -f "$OUTPUT" && "$OUTPUT" -nt "$INPUT" \
      && "$OUTPUT" -nt "$ROOT_DIR/scripts/build-native-app-aot.sh" \
      && "$OUTPUT" -nt "$ROOT_DIR/third_party/versions.env" ]]; then
  echo "AOT output is current: $OUTPUT"
  exit 0
fi

"$ROOT_DIR/scripts/build-wamrc.sh"

WAMRC_ARGS=(
  --target=armv7a-pc-linux-eabi
  --disable-simd
  --bounds-checks=1
  -o "$OUTPUT"
  "$INPUT"
)

if [[ -n "$BUILD_CONTAINER" ]]; then
  distrobox enter "$BUILD_CONTAINER" -- "$WAMRC" "${WAMRC_ARGS[@]}"
else
  "$WAMRC" "${WAMRC_ARGS[@]}"
fi

echo "Built $OUTPUT"

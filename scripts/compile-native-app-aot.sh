#!/usr/bin/env bash

set -euo pipefail

if (( $# != 2 )); then
  echo "usage: $0 INPUT.wasm OUTPUT.aot" >&2
  exit 2
fi

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
INPUT=$(realpath "$1")
OUTPUT=$(realpath -m "$2")

if [[ -f "$ROOT_DIR/.env" ]]; then
  set -a
  source "$ROOT_DIR/.env"
  set +a
fi

"$ROOT_DIR/scripts/build-wamrc.sh"
WAMRC="$ROOT_DIR/build/host-wamrc/wamrc"
BUILD_CONTAINER=${OOS_WAMR_DISTROBOX:-}
mkdir -p "$(dirname "$OUTPUT")"

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

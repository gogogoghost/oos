#!/usr/bin/env bash

set -euo pipefail

if (( $# != 2 )); then
  echo "usage: $0 INPUT.wasm OUTPUT.TARGET.aot" >&2
  exit 2
fi

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
INPUT=$(realpath "$1")
OUTPUT=$(realpath -m "$2")
OUTPUT_NAME=${OUTPUT##*/}
OUTPUT_STEM=${OUTPUT_NAME%.aot}
TARGET_NAME=${OUTPUT_STEM##*.}

case "$TARGET_NAME" in
  armv7a)
    WAMRC_TARGET=armv7a-pc-linux-eabi
    CPU_ARGS=()
    ;;
  cortex-a7|cortex-a53)
    WAMRC_TARGET=armv7a-pc-linux-eabi
    CPU_ARGS=("--cpu=$TARGET_NAME")
    ;;
  aarch64)
    WAMRC_TARGET=aarch64-pc-linux-gnu
    CPU_ARGS=()
    ;;
  x86_64)
    WAMRC_TARGET=x86_64-pc-linux-gnu
    CPU_ARGS=()
    ;;
  *)
    echo "unsupported AOT target suffix: $TARGET_NAME" >&2
    exit 2
    ;;
esac

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
  "--target=$WAMRC_TARGET"
  "${CPU_ARGS[@]}"
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

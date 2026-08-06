#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
INPUT_DIR="$ROOT_DIR/build/native-apps/subruntime-host-modules"
OUTPUT_DIR="$ROOT_DIR/build/native-apps/subruntime-armv7-modules"

"$ROOT_DIR/scripts/build-c-subruntime-smoke.sh"
mkdir -p "$OUTPUT_DIR"
for module in memory-child allocation-failure trap-child stack-overflow; do
  "$ROOT_DIR/scripts/compile-native-app-aot.sh" \
    "$INPUT_DIR/$module.wasm" "$OUTPUT_DIR/$module.aot"
done
"$ROOT_DIR/scripts/compile-native-app-aot.sh" \
  "$ROOT_DIR/build/native-apps/c-subruntime-parent.wasm" \
  "$ROOT_DIR/build/native-apps/c-subruntime-parent.aot"

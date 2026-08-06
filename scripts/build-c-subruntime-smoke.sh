#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUTPUT_DIR="$ROOT_DIR/build/native-apps"
MODULE_DIR="$OUTPUT_DIR/subruntime-host-modules"
mkdir -p "$MODULE_DIR"
OOS_WASM_MEMORY_BYTES=33554432 \
  "$ROOT_DIR/scripts/build-c-app.sh" \
  "$MODULE_DIR/memory-child.wasm" \
  "$ROOT_DIR/sdk/c/tests/subruntime_child.c"
OOS_WASM_MEMORY_BYTES=16777216 \
  "$ROOT_DIR/scripts/build-c-app.sh" \
  "$MODULE_DIR/allocation-failure.wasm" \
  "$ROOT_DIR/sdk/c/tests/subruntime_allocation_failure.c"
"$ROOT_DIR/scripts/build-c-app.sh" \
  "$MODULE_DIR/trap-child.wasm" \
  "$ROOT_DIR/sdk/c/tests/worker_wit_trap.c"
"$ROOT_DIR/scripts/build-c-app.sh" \
  "$MODULE_DIR/stack-overflow.wasm" \
  "$ROOT_DIR/sdk/c/tests/subruntime_stack_overflow.c"
"$ROOT_DIR/scripts/build-c-app.sh" "$OUTPUT_DIR/c-subruntime-parent.wasm" \
  "$ROOT_DIR/sdk/c/tests/subruntime_parent.c"

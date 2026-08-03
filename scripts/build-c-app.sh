#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
[[ $# -ge 2 ]] || {
  echo "usage: $0 OUTPUT.wasm SOURCE.c [SOURCE.c ...]" >&2
  exit 2
}
OUTPUT=$1
shift
SYSROOT="$ROOT_DIR/third_party/wasm-micro-runtime/wamr-sdk/app/libc-builtin-sysroot"
mkdir -p "$(dirname "$OUTPUT")"
clang --target=wasm32 -Oz -nostdlib -pthread -ffunction-sections \
  --sysroot="$SYSROOT" -I"$ROOT_DIR/sdk/c/tests/wasm-stdlib" \
  -I"$ROOT_DIR/sdk/c/generated" \
  "$ROOT_DIR/sdk/c/generated/app.c" \
  "$ROOT_DIR/sdk/c/generated/app_component_type.o" \
  "$ROOT_DIR/sdk/c/runtime/oos_allocator.c" "$@" \
  -Wl,--no-entry,--gc-sections,--strip-all,--allow-undefined \
  -Wl,--shared-memory,--initial-memory=1048576,--max-memory=67108864 \
  -Wl,-z,stack-size=262144 \
  -Wl,--export=__heap_base,--export=__data_end \
  -o "$OUTPUT"
echo "Built $OUTPUT"

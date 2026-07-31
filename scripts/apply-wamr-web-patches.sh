#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WAMR_ROOT="$ROOT_DIR/third_party/wasm-micro-runtime"
WAMR_C_API="$WAMR_ROOT/core/iwasm/common/wasm_c_api.c"

apply_patch_file() {
  local patch_file=$1
  local description=$2

  if patch --batch --silent --dry-run -d "$WAMR_ROOT" -p1 <"$patch_file"; then
    patch --batch --silent -d "$WAMR_ROOT" -p1 <"$patch_file"
    echo "Applied $description"
    return
  fi
  echo "Pinned WAMR source does not match $(basename "$patch_file")" >&2
  exit 1
}

if grep -Fq 'imported_memory_interp->u.memory.is_linked = true;' \
    "$WAMR_C_API" &&
   grep -Fq '&& !memory->is_linked' \
    "$WAMR_ROOT/core/iwasm/interpreter/wasm_runtime.c"; then
  echo "WAMR C API imported-memory patch is ready"
else
  apply_patch_file "$ROOT_DIR/system/patches/wamr-c-api-import-memory.patch" \
    "WAMR C API imported-memory patch"
fi

if grep -Fq 'wasm_instance_delete_internal(owned_instance);' "$WAMR_C_API"; then
  echo "WAMR C API instance-lifetime patch is ready"
else
  apply_patch_file \
    "$ROOT_DIR/system/patches/wamr-c-api-instance-lifetime.patch" \
    "WAMR C API instance-lifetime patch"
fi

if grep -Fq 'fast-interpreter loader uses a zeroed placeholder instance' \
    "$WAMR_ROOT/core/iwasm/common/wasm_runtime_common.c"; then
  echo "WAMR configurable-bounds null-instance patch is ready"
else
  apply_patch_file \
    "$ROOT_DIR/system/patches/wamr-configurable-bounds-null-instance.patch" \
    "WAMR configurable-bounds null-instance patch"
fi

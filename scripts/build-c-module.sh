#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [[ -f "$ROOT_DIR/.env" ]]; then
  set -a
  source "$ROOT_DIR/.env"
  set +a
fi
[[ $# -ge 2 ]] || {
  echo "usage: $0 OUTPUT.wasm SOURCE.c [SOURCE.c ...]" >&2
  exit 2
}
OUTPUT=$(realpath -m "$1")
shift
mkdir -p "$(dirname "$OUTPUT")"
"$ROOT_DIR/scripts/fetch-tlsf.sh"
"$ROOT_DIR/scripts/build-wasm-picolibc.sh"
PICOLIBC_DIR="$ROOT_DIR/build/wasm-picolibc/install"
MEMORY_BYTES=${OOS_WASM_MODULE_MEMORY_BYTES:-16777216}
INITIAL_MEMORY_BYTES=${OOS_WASM_MODULE_INITIAL_MEMORY_BYTES:-2097152}
BUILD_CONTAINER=${OOS_WAMR_DISTROBOX:-}

build() {
  local compiler=${CC:-clang}
  if [[ -n ${OOS_WASI_SDK:-} ]]; then
    compiler="$OOS_WASI_SDK/bin/clang"
  fi
  [[ -x "$compiler" ]] || {
    echo "C guest compiler is unavailable: $compiler" >&2
    exit 1
  }
  local object_directory="${OUTPUT}.objects"
  local tlsf_object="$object_directory/tlsf.o"
  mkdir -p "$object_directory"
  "$compiler" --target=wasm32-unknown-unknown -O2 -flto -nostdlib \
    -matomics -mbulk-memory -ffunction-sections -fdata-sections \
    -D__IEEE_LITTLE_ENDIAN -DNDEBUG -Dprintf=oos_tlsf_diagnostic \
    -I"$ROOT_DIR/third_party/tlsf" -isystem "$PICOLIBC_DIR/include" \
    -c "$ROOT_DIR/third_party/tlsf/tlsf.c" -o "$tlsf_object"
  local sources=("$ROOT_DIR/sdk/c/generated/module.c"
                 "$ROOT_DIR/sdk/c/runtime/oos_allocator.c" "$@")
  local objects=("$ROOT_DIR/sdk/c/generated/module_component_type.o"
                 "$tlsf_object")
  local index=0
  for source in "${sources[@]}"; do
    local object="$object_directory/source-$index.o"
    "$compiler" --target=wasm32-unknown-unknown -O2 -flto -nostdlib \
      -matomics -mbulk-memory -ffunction-sections -fdata-sections \
      -D__IEEE_LITTLE_ENDIAN \
      -I"$ROOT_DIR/sdk/c/include" -I"$ROOT_DIR/sdk/c/generated" \
      -I"$ROOT_DIR/third_party/tlsf" -isystem "$PICOLIBC_DIR/include" \
      -c "$source" -o "$object"
    objects+=("$object")
    index=$((index + 1))
  done
  local compiler_rt
  compiler_rt=$("$compiler" --target=wasm32-wasi --print-resource-dir)
  compiler_rt="$compiler_rt/lib/wasi/libclang_rt.builtins-wasm32.a"
  "$compiler" --target=wasm32-unknown-unknown -O0 -flto -nostdlib \
    -matomics -mbulk-memory "${objects[@]}" \
    -Wl,--no-entry,--gc-sections,--strip-all,--fatal-warnings \
    -Wl,--no-check-features -Xlinker --features=atomics,bulk-memory \
    -Wl,--shared-memory,--initial-memory="$INITIAL_MEMORY_BYTES" \
    -Wl,--max-memory="$MEMORY_BYTES" -Wl,-z,stack-size=262144 \
    -Wl,--export=__heap_base,--export=__data_end \
    -L"$PICOLIBC_DIR/lib" -lm -lc -lm -lc "$compiler_rt" -o "$OUTPUT"
  "$ROOT_DIR/scripts/validate-wasm-imports.sh" "$OUTPUT"
}

if [[ -n "$BUILD_CONTAINER" && "${OOS_IN_DISTROBOX:-0}" != 1 ]]; then
  exec distrobox enter "$BUILD_CONTAINER" -- env OOS_IN_DISTROBOX=1 \
    OOS_WASM_MODULE_MEMORY_BYTES="$MEMORY_BYTES" \
    OOS_WASM_MODULE_INITIAL_MEMORY_BYTES="$INITIAL_MEMORY_BYTES" \
    "$0" "$OUTPUT" "$@"
fi
build "$@"
echo "Built module $OUTPUT"

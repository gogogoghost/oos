#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [[ -f "$ROOT_DIR/.env" ]]; then
  set -a
  source "$ROOT_DIR/.env"
  set +a
fi
[[ $# -ge 2 ]] || {
  echo "usage: $0 OUTPUT.wasm SOURCE.[c|cpp] [SOURCE ...]" >&2
  exit 2
}
OUTPUT=$(realpath -m "$1")
shift
mkdir -p "$(dirname "$OUTPUT")"
"$ROOT_DIR/scripts/fetch-tlsf.sh"
"$ROOT_DIR/scripts/build-wasm-picolibc.sh"

[[ -n ${OOS_WASI_SDK:-} && -x "$OOS_WASI_SDK/bin/clang++" ]] || {
  echo "OOS_WASI_SDK with clang++ is required for C++ Wasm applications" >&2
  exit 1
}

CLANG="$OOS_WASI_SDK/bin/clang"
CLANGXX="$OOS_WASI_SDK/bin/clang++"
PICOLIBC_DIR="$ROOT_DIR/build/wasm-picolibc/install"
WASI_SYSROOT="$OOS_WASI_SDK/share/wasi-sysroot"
CPP_INCLUDE="$WASI_SYSROOT/include/wasm32-wasi/c++/v1"
CPP_LIB="$WASI_SYSROOT/lib/wasm32-wasi"
RESOURCE_DIR=$("$CLANG" --target=wasm32-wasi --print-resource-dir)
WORKER_STACK_BYTES=2097152
MEMORY_BYTES=${OOS_WASM_MEMORY_BYTES:-67108864}
INITIAL_MEMORY_BYTES=${OOS_WASM_INITIAL_MEMORY_BYTES:-8388608}
TOTAL_STACK_BYTES=$((2 * WORKER_STACK_BYTES))
if (( TOTAL_STACK_BYTES >= INITIAL_MEMORY_BYTES ||
      INITIAL_MEMORY_BYTES > MEMORY_BYTES )); then
  echo "invalid OOS Wasm stack/memory policy" >&2
  exit 2
fi

OBJECT_DIR="${OUTPUT}.objects"
mkdir -p "$OBJECT_DIR"
CPP_LIBC="$OBJECT_DIR/libc-cpp.a"
cp "$PICOLIBC_DIR/lib/libc.a" "$CPP_LIBC"
"$OOS_WASI_SDK/bin/llvm-ar" d "$CPP_LIBC" \
  nano-calloc.c.o nano-free.c.o nano-malloc.c.o nano-realloc.c.o
COMMON_FLAGS=(
  --target=wasm32-wasi -O2 -flto -pthread -nostdlib
  -matomics -mbulk-memory -ffunction-sections -fdata-sections
  -D__IEEE_LITTLE_ENDIAN -DOOS_WASM_GUEST
  -I"$ROOT_DIR/sdk/c/include"
  -I"$ROOT_DIR/sdk/c/generated"
  -I"$ROOT_DIR/sdk/cpp/guest/include"
  -I"$ROOT_DIR/sdk/cpp/include"
  -I"$ROOT_DIR/system/src"
  -I"$ROOT_DIR/third_party/tlsf"
  -isystem "$PICOLIBC_DIR/include"
  -isystem "$RESOURCE_DIR/include")
CPP_FLAGS=(-nostdinc++ -isystem "$CPP_INCLUDE" -fno-exceptions -fno-rtti)
if [[ -n ${OOS_LV_CONF_DIR:-} ]]; then
  [[ -f "$OOS_LV_CONF_DIR/lv_conf.h" ]] || {
    echo "LVGL configuration is unavailable: $OOS_LV_CONF_DIR/lv_conf.h" >&2
    exit 1
  }
  COMMON_FLAGS+=(
    -DLV_CONF_INCLUDE_SIMPLE
    -I"$OOS_LV_CONF_DIR"
    -I"$ROOT_DIR/sdk/c/lvgl"
    -I"$ROOT_DIR/third_party/lvgl")
fi
if [[ -n ${OOS_GUEST_INCLUDE_DIRS:-} ]]; then
  IFS=: read -r -a guest_include_dirs <<<"$OOS_GUEST_INCLUDE_DIRS"
  for include_dir in "${guest_include_dirs[@]}"; do
    [[ -d "$include_dir" ]] || {
      echo "guest include directory is unavailable: $include_dir" >&2
      exit 1
    }
    COMMON_FLAGS+=(-I"$include_dir")
  done
fi

SOURCES=(
  "$ROOT_DIR/sdk/c/generated/app.c"
  "$ROOT_DIR/sdk/c/runtime/oos_allocator.c"
  "$ROOT_DIR/third_party/tlsf/tlsf.c"
  "$@")
OBJECTS=("$ROOT_DIR/sdk/c/generated/app_component_type.o")
index=0
for source in "${SOURCES[@]}"; do
  object="$OBJECT_DIR/source-$index.o"
  case "$source" in
  *.cpp|*.cc|*.cxx)
    "$CLANGXX" "${CPP_FLAGS[@]}" "${COMMON_FLAGS[@]}" -c "$source" -o "$object"
    ;;
  *)
    "$CLANG" "${COMMON_FLAGS[@]}" -c "$source" -o "$object"
    ;;
  esac
  OBJECTS+=("$object")
  index=$((index + 1))
done

"$CLANGXX" --target=wasm32-wasi -O0 -flto -nostdlib \
  -matomics -mbulk-memory "${OBJECTS[@]}" \
  -Wl,--no-entry,--gc-sections,--strip-all,--fatal-warnings \
  -Wl,--no-check-features -Xlinker --features=atomics,bulk-memory \
  -Wl,--shared-memory,--initial-memory="$INITIAL_MEMORY_BYTES" \
  -Wl,--max-memory="$MEMORY_BYTES" -Wl,-z,stack-size="$TOTAL_STACK_BYTES" \
  -Wl,--export=__heap_base,--export=__data_end \
  "$CPP_LIB/libc++.a" "$CPP_LIB/libc++abi.a" \
  -L"$PICOLIBC_DIR/lib" -lm "$CPP_LIBC" -ldummyhost -lm "$CPP_LIBC" \
  -ldummyhost \
  "$RESOURCE_DIR/lib/wasi/libclang_rt.builtins-wasm32.a" \
  -o "$OUTPUT"
"$ROOT_DIR/scripts/validate-wasm-imports.sh" "$OUTPUT"
echo "Built $OUTPUT"

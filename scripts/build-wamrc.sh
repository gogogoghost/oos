#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [[ -f "$ROOT_DIR/.env" ]]; then
  set -a
  source "$ROOT_DIR/.env"
  set +a
fi

BUILD_CONTAINER=${OOS_WAMR_DISTROBOX:-}
if [[ -n "$BUILD_CONTAINER" && "${OOS_IN_DISTROBOX:-0}" != 1 ]]; then
  exec distrobox enter "$BUILD_CONTAINER" -- env OOS_IN_DISTROBOX=1 \
    bash "$ROOT_DIR/scripts/build-wamrc.sh"
fi

"$ROOT_DIR/scripts/fetch-wamr.sh"
WAMR_DIR="$ROOT_DIR/third_party/wasm-micro-runtime"
BUILD_DIR="$ROOT_DIR/build/host-wamrc"
WAMR_AOT_C_API_PATCH="$ROOT_DIR/system/patches/wamr-aot-c-api-import-arm32.patch"
WAMR_AOT_CPU_PATCH="$ROOT_DIR/system/patches/wamr-aot-explicit-triple-cpu.patch"

if grep -Fq 'INT8_PTR_TYPE, "c_api_params"' \
    "$WAMR_DIR/core/iwasm/compilation/aot_emit_function.c"; then
  echo "WAMR ARM32 C API import compiler patch is ready"
elif patch --batch --silent --dry-run -d "$WAMR_DIR" -p1 \
    <"$WAMR_AOT_C_API_PATCH"; then
  patch --batch --silent -d "$WAMR_DIR" -p1 <"$WAMR_AOT_C_API_PATCH"
  echo "Applied WAMR ARM32 C API import compiler patch"
else
  echo "Pinned WAMR source does not match $(basename "$WAMR_AOT_C_API_PATCH")" >&2
  exit 1
fi

if grep -Fq 'Keep CPU tuning with an explicit target triple' \
    "$WAMR_DIR/core/iwasm/compilation/aot_llvm.c"; then
  echo "WAMR explicit-triple CPU tuning patch is ready"
elif patch --batch --silent --dry-run -d "$WAMR_DIR" -p1 \
    <"$WAMR_AOT_CPU_PATCH"; then
  patch --batch --silent -d "$WAMR_DIR" -p1 <"$WAMR_AOT_CPU_PATCH"
  echo "Applied WAMR explicit-triple CPU tuning patch"
else
  echo "Pinned WAMR source does not match $(basename "$WAMR_AOT_CPU_PATCH")" >&2
  exit 1
fi

if [[ -n "${OOS_LLVM_CONFIG:-}" ]]; then
  LLVM_CONFIG=$OOS_LLVM_CONFIG
elif command -v llvm-config-14 >/dev/null 2>&1; then
  LLVM_CONFIG=$(command -v llvm-config-14)
elif command -v llvm-config >/dev/null 2>&1; then
  LLVM_CONFIG=$(command -v llvm-config)
else
  echo "LLVM is required. Install llvm-14-dev clang-14 lld-14." >&2
  exit 1
fi
LLVM_VERSION=$($LLVM_CONFIG --version)
LLVM_MAJOR=${LLVM_VERSION%%.*}
if (( LLVM_MAJOR > 20 )); then
  echo "WAMR 2.4.4 is not compatible with LLVM $LLVM_VERSION." >&2
  echo "Install LLVM 14 and ensure llvm-config-14 is available." >&2
  exit 1
fi
LLVM_CMAKE_DIR=$($LLVM_CONFIG --cmakedir)

cmake -S "$WAMR_DIR/wamr-compiler" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DWAMR_BUILD_WITH_CUSTOM_LLVM=1 \
  -DLLVM_DIR="$LLVM_CMAKE_DIR" \
  -DWAMR_BUILD_SIMD=0
cmake --build "$BUILD_DIR"

test -x "$BUILD_DIR/wamrc"
echo "Built $BUILD_DIR/wamrc"

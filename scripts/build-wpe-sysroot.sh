#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ENV_FILE="$ROOT_DIR/.env"

if [[ -f "$ENV_FILE" ]]; then
  set -a
  source "$ENV_FILE"
  set +a
fi

if [[ ${OOS_WPE_IN_DISTROBOX:-0} != 1 && -n ${WPE_DISTROBOX:-} ]]; then
  if ! command -v distrobox >/dev/null 2>&1; then
    echo "WPE_DISTROBOX is set but distrobox is not installed." >&2
    exit 1
  fi
  exec distrobox enter "$WPE_DISTROBOX" -- \
    env OOS_WPE_IN_DISTROBOX=1 "$ROOT_DIR/scripts/build-wpe-sysroot.sh" "$@"
fi

WPE_CERBERO_DIR=${WPE_CERBERO_DIR:-"$ROOT_DIR/third_party/wpe-android-cerbero"}
WPE_NDK=${WPE_NDK:-/home/jax/Android/Sdk/ndk/magisk}
WPEWEBKIT_COMMIT=${WPEWEBKIT_COMMIT:-9d11fa1a37e61a75d8167ee4bc1a8e7604aff408}
WPE_BUILD_JOBS=${WPE_BUILD_JOBS:-$(nproc)}
BASE_CONFIG="$WPE_CERBERO_DIR/config/cross-android-armv7.cbc"
KAIOS_CONFIG="$ROOT_DIR/config/cerbero/kaios-android29-armv7.cbc"
CERBERO="$WPE_CERBERO_DIR/cerbero-uninstalled"
BACKEND_PATCH="$ROOT_DIR/patches/wpebackend-android-rgb565.patch"
BACKEND_SOURCE="$WPE_CERBERO_DIR/build/sources/android_armv7/wpebackend-android"
WPE_BUILD_DIR="$WPE_CERBERO_DIR/build/sources/android_armv7/wpewebkit-git/b"

if ! python3 -c 'import distro' >/dev/null 2>&1; then
  echo "Missing Python module distro. Install Debian package python3-distro." >&2
  exit 1
fi
if [[ ! -x "$CERBERO" || ! -f "$BASE_CONFIG" ]]; then
  echo "Missing WPE Android Cerbero checkout: $WPE_CERBERO_DIR" >&2
  exit 1
fi
if [[ ! -f "$WPE_NDK/build/cmake/android.toolchain.cmake" ]]; then
  echo "WPE_NDK is not an Android NDK: $WPE_NDK" >&2
  exit 1
fi

export KAIOS_WPE_NDK="$WPE_NDK"
export WPEWEBKIT_COMMIT

"$ROOT_DIR/scripts/apply-wpe-cerbero-patches.sh"
BOOTSTRAP_ARGS=(--system=no)
if [[ -x "$WPE_CERBERO_DIR/build/rust/cargo/bin/rustc" ]]; then
  BOOTSTRAP_ARGS+=(--toolchains=no)
fi
"$CERBERO" -c "$BASE_CONFIG" -c "$KAIOS_CONFIG" bootstrap "${BOOTSTRAP_ARGS[@]}"
if [[ ! -d "$BACKEND_SOURCE/.git" ]]; then
  "$CERBERO" -c "$BASE_CONFIG" -c "$KAIOS_CONFIG" buildone \
    --steps fetch extract -- wpebackend-android
fi
if git -C "$BACKEND_SOURCE" apply --reverse --check "$BACKEND_PATCH" >/dev/null 2>&1; then
  echo "WPE Android RGB565 patch already applied."
elif git -C "$BACKEND_SOURCE" apply --check "$BACKEND_PATCH" >/dev/null 2>&1; then
  git -C "$BACKEND_SOURCE" apply "$BACKEND_PATCH"
  echo "Applied WPE Android RGB565 patch."
else
  echo "WPE Android backend source does not match RGB565 patch." >&2
  exit 1
fi
"$CERBERO" -c "$BASE_CONFIG" -c "$KAIOS_CONFIG" buildone -j "$WPE_BUILD_JOBS" \
  --steps configure compile install -- wpebackend-android
"$CERBERO" -c "$BASE_CONFIG" -c "$KAIOS_CONFIG" buildone -j "$WPE_BUILD_JOBS" \
  --steps fetch extract configure compile install -- wpewebkit

verify_define() {
  local name=$1
  local expected=$2
  grep -q "^#define $name $expected$" "$WPE_BUILD_DIR/cmakeconfig.h" || {
    echo "WPE feature verification failed: expected $name=$expected" >&2
    exit 1
  }
}

verify_define ENABLE_JIT 1
verify_define ENABLE_DFG_JIT 1
verify_define ENABLE_WEBASSEMBLY 1
verify_define ENABLE_C_LOOP 0
verify_define ENABLE_FTL_JIT 0

for jit_object in \
  Source/JavaScriptCore/CMakeFiles/JavaScriptCore.dir/dfg/DFGSpeculativeJIT32_64.cpp.o \
  Source/JavaScriptCore/CMakeFiles/JavaScriptCore.dir/wasm/WasmBBQJIT32_64.cpp.o; do
  if [[ ! -f "$WPE_BUILD_DIR/$jit_object" ]]; then
    echo "WPE feature verification failed: missing $jit_object" >&2
    exit 1
  fi
done
echo "Verified ARMv7 Baseline/DFG JIT and WebAssembly BBQ JIT build artifacts."

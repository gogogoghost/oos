#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [[ -f "$ROOT_DIR/.env" ]]; then
  set -a
  source "$ROOT_DIR/.env"
  set +a
fi
PROFILE=${1:-local}
JOBS=${WAMR_WEB_BUILD_JOBS:-$(nproc)}
export CCACHE_DISABLE=${CCACHE_DISABLE:-1}
source "$ROOT_DIR/third_party/versions.env"
WAMR_RELEASE=${WAMR_VERSION#WAMR-}
UNSAFE_FAST=${OOS_WAMR_WEB_UNSAFE_FAST:-OFF}

case "$UNSAFE_FAST" in
  ON|1|true|TRUE) UNSAFE_FAST=ON ;;
  OFF|0|false|FALSE) UNSAFE_FAST=OFF ;;
  *)
    echo "OOS_WAMR_WEB_UNSAFE_FAST must be ON or OFF" >&2
    exit 2
    ;;
esac

case "$PROFILE" in
  local)
    WPE_SYSROOT=${OOS_WPE_LOCAL_SYSROOT:-"$ROOT_DIR/build/wpe-sysroot/local-root"}
    WPE_PREFIX="$WPE_SYSROOT/opt/oos"
    WAMR_PLATFORM=linux
    WAMR_TARGET=X86_64
    if [[ $UNSAFE_FAST == ON ]]; then
      AOT_NAMESPACE="wamr-$WAMR_RELEASE/x86_64-nosimd-no-bounds"
    else
      AOT_NAMESPACE="wamr-$WAMR_RELEASE/x86_64-nosimd-bounds-checks"
    fi
    ENABLE_JIT=${OOS_WAMR_WEB_JIT:-ON}
    TOOLCHAIN_ARGS=()
    ;;
  nokia-2780-flip|nokia-8110-4g)
    DEVICE_CONFIG="$ROOT_DIR/system/config/wpe/devices/$PROFILE.env"
    source "$DEVICE_CONFIG"
    WPE_PREFIX="$ROOT_DIR/build/wpe-sysroot/$OOS_WPE_SYSROOT_KEY"
    WAMR_PLATFORM=android
    WAMR_TARGET=ARMV7A
    AOT_CPU=${OOS_WAMR_AOT_CPU:-generic}
    if [[ $AOT_CPU == generic && $UNSAFE_FAST == OFF ]]; then
      # Preserve the namespace used by existing generic, checked artifacts.
      AOT_NAMESPACE="wamr-$WAMR_RELEASE/armv7a-nosimd-bounds-checks"
    elif [[ $UNSAFE_FAST == ON ]]; then
      AOT_NAMESPACE="wamr-$WAMR_RELEASE/armv7-$AOT_CPU-nosimd-no-bounds"
    else
      AOT_NAMESPACE="wamr-$WAMR_RELEASE/armv7-$AOT_CPU-nosimd-bounds-checks"
    fi
    ENABLE_JIT=${OOS_WAMR_WEB_JIT:-OFF}
    WPE_NDK=${WPE_NDK:-/home/jax/Android/Sdk/ndk/magisk}
    TOOLCHAIN_FILE="$WPE_NDK/build/cmake/android.toolchain.cmake"
    [[ -f "$TOOLCHAIN_FILE" ]] || {
      echo "WPE_NDK is not an Android NDK: $WPE_NDK" >&2
      exit 1
    }
    TOOLCHAIN_ARGS=(
      -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
      -DANDROID_ABI=armeabi-v7a
      -DANDROID_PLATFORM="android-$OOS_WPE_ANDROID_API"
      -DANDROID_STL=c++_shared)
    ;;
  *)
    echo "Unsupported WAMR WebProcess profile: $PROFILE" >&2
    exit 2
    ;;
esac
BUILD_DIR=${OOS_WAMR_WEB_BUILD_DIR:-"$ROOT_DIR/build/wamr-web/$PROFILE"}

[[ -f "$WPE_PREFIX/lib/libWPEWebKit-2.0.so" ]] || {
  echo "The $PROFILE WPE sysroot is missing. Build it before the WAMR extension." >&2
  exit 1
}

"$ROOT_DIR/scripts/apply-wamr-web-patches.sh"

cmake_args=(
  -S "$ROOT_DIR/system/runtime/wamr-web"
  -B "$BUILD_DIR"
  -G Ninja
  -DCMAKE_BUILD_TYPE=Release
  -DOOS_REPO_ROOT="$ROOT_DIR"
  -DOOS_WPE_PREFIX="$WPE_PREFIX"
  -DOOS_WAMR_WEB_PLATFORM="$WAMR_PLATFORM"
  -DOOS_WAMR_WEB_TARGET="$WAMR_TARGET"
  -DOOS_WAMR_WEB_AOT_NAMESPACE="$AOT_NAMESPACE"
  -DOOS_WAMR_WEB_UNSAFE_FAST="$UNSAFE_FAST"
  -DOOS_WAMR_WEB_JIT="$ENABLE_JIT")
cmake_args+=("${TOOLCHAIN_ARGS[@]}")

if [[ $ENABLE_JIT == ON ]]; then
  llvm_config=${OOS_LLVM_CONFIG:-}
  if [[ -z $llvm_config ]]; then
    for candidate in llvm-config-20 llvm-config-19 llvm-config-18 llvm-config; do
      if command -v "$candidate" >/dev/null 2>&1; then
        llvm_config=$(command -v "$candidate")
        break
      fi
    done
  fi
  if [[ -z $llvm_config ]]; then
    echo "LLVM JIT development files are missing (Fedora: llvm19-devel)." >&2
    echo "Set OOS_WAMR_WEB_JIT=OFF to build the interpreter/AOT profile." >&2
    exit 1
  fi
  llvm_cmake_dir=$($llvm_config --cmakedir)
  cmake_args+=("-DLLVM_DIR=$llvm_cmake_dir")
fi

cmake "${cmake_args[@]}"
cmake --build "$BUILD_DIR" -j "$JOBS"
cmake --install "$BUILD_DIR" --prefix "$WPE_PREFIX"

echo "WAMR WebProcess extension installed in $WPE_PREFIX"

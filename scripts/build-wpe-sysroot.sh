#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ENV_FILE="$ROOT_DIR/.env"
DEVICE=${1:-nokia-2780-flip}
DEVICE_CONFIG="$ROOT_DIR/system/config/wpe/devices/$DEVICE.env"

if [[ -f "$ENV_FILE" ]]; then
  set -a
  source "$ENV_FILE"
  set +a
fi

if [[ ! -f "$DEVICE_CONFIG" ]]; then
  echo "Unsupported WPE device: $DEVICE (missing $DEVICE_CONFIG)" >&2
  exit 2
fi
source "$DEVICE_CONFIG"

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
WPE_BUILD_JOBS=${WPE_BUILD_JOBS:-${OOS_WPE_DEFAULT_JOBS:-$(nproc)}}
export WPE_BUILD_JOBS
BASE_CONFIG="$WPE_CERBERO_DIR/config/cross-android-armv7.cbc"
KAIOS_CONFIG="$ROOT_DIR/$OOS_WPE_CERBERO_CONFIG"
CERBERO="$WPE_CERBERO_DIR/cerbero-uninstalled"
PROFILE_SOURCE="$WPE_CERBERO_DIR/build/sources/$OOS_WPE_SOURCE_KEY"
WPE_BUILD_DIR="$PROFILE_SOURCE/wpewebkit-git/b"
WPE_PREFIX="$ROOT_DIR/build/wpe-sysroot/$OOS_WPE_SYSROOT_KEY"

case "$OOS_WPE_PROFILE" in
  android29-armv7-jit)
    [[ $OOS_WPE_ANDROID_API == 29 && $OOS_WPE_SOURCE_KEY == android_armv7 ]] || {
      echo "Invalid Android 29 WPE profile mapping" >&2
      exit 2
    }
    ;;
  android23-armv7-jit)
    [[ $OOS_WPE_ANDROID_API == 23 && $OOS_WPE_SOURCE_KEY == android23-armv7-jit ]] || {
      echo "Invalid Android 23 WPE profile mapping" >&2
      exit 2
    }
    ;;
  *)
    echo "Unknown WPE build profile: $OOS_WPE_PROFILE" >&2
    exit 2
    ;;
esac

if [[ $PROFILE_SOURCE == "$WPE_CERBERO_DIR/build/sources/android_armv7" && \
      $OOS_WPE_ANDROID_API != 29 ]]; then
  echo "Refusing to mix an API $OOS_WPE_ANDROID_API build into the API 29 source tree" >&2
  exit 2
fi

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
export OOS_WPE_PREFIX="$WPE_PREFIX"

mkdir -p "$WPE_PREFIX"
printf '%s\n' \
  "device=$DEVICE" \
  "profile=$OOS_WPE_PROFILE" \
  "android_api=$OOS_WPE_ANDROID_API" \
  "buffer_abi=$OOS_WPE_BUFFER_ABI" \
  "display_backend=$OOS_WPE_DISPLAY_BACKEND" \
  "webkit_commit=$WPEWEBKIT_COMMIT" \
  >"$WPE_PREFIX/.oos-wpe-profile.pending"

if [[ $OOS_WPE_ANDROID_API -lt 26 ]]; then
  "$ROOT_DIR/scripts/configure-android.sh" "$DEVICE" -DOOS_BUILD_DEVICE_TESTS=OFF
  cmake --build "$ROOT_DIR/build/android-$DEVICE" \
    --target oos_android23_hardware_buffer -j "$WPE_BUILD_JOBS"
  mkdir -p "$WPE_PREFIX/lib"
  install -m 0755 \
    "$ROOT_DIR/build/android-$DEVICE/devices/nokia-8110-4g/liboos-android23-buffer.so" \
    "$WPE_PREFIX/lib/liboos-android23-buffer.so"
fi

"$ROOT_DIR/scripts/apply-wpe-cerbero-patches.sh"
BOOTSTRAP_ARGS=(--system=no)
if [[ -x "$WPE_CERBERO_DIR/build/rust/cargo/bin/rustc" ]]; then
  BOOTSTRAP_ARGS+=(--toolchains=no)
fi
"$CERBERO" -c "$BASE_CONFIG" -c "$KAIOS_CONFIG" bootstrap "${BOOTSTRAP_ARGS[@]}"
"$CERBERO" -c "$BASE_CONFIG" -c "$KAIOS_CONFIG" build -j "$WPE_BUILD_JOBS" \
  wpebackend-android
"$CERBERO" -c "$BASE_CONFIG" -c "$KAIOS_CONFIG" build -j "$WPE_BUILD_JOBS" \
  wpewebkit

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
mv "$WPE_PREFIX/.oos-wpe-profile.pending" "$WPE_PREFIX/.oos-wpe-profile"
echo "WPE sysroot ready: device=$DEVICE profile=$OOS_WPE_PROFILE prefix=$WPE_PREFIX"

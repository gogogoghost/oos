#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DEVICE=${1:-nokia-2780-flip}
if [[ $# -gt 0 ]]; then
  shift
fi
ENV_FILE="$ROOT_DIR/.env"

if [[ ! -f "$ENV_FILE" ]]; then
  echo "Missing $ENV_FILE. Copy .env.example and set the device sysroot paths." >&2
  exit 1
fi

set -a
source "$ENV_FILE"
set +a

GENERATE_HIDL=0
ANDROID_PLATFORM=android-29
DEVICE_OPTIONS=()
case "$DEVICE" in
  nokia-2780-flip)
    SYSTEM_DIR=${NOKIA_2780_SYSTEM_DIR:-}
    GENERATE_HIDL=1
    DEVICE_OPTIONS+=(
      -DBUILD_NOKIA_2780_FLIP=ON
      -DBUILD_NOKIA_8110_4G=OFF)
    ;;
  nokia-8110-4g)
    SYSTEM_DIR=${NOKIA_8110_SYSTEM_DIR:-}
    ANDROID_PLATFORM=android-23
    DEVICE_OPTIONS+=(
      -DBUILD_NOKIA_2780_FLIP=OFF
      -DBUILD_NOKIA_8110_4G=ON)
    ;;
  *)
    echo "Unknown device: $DEVICE" >&2
    exit 1
    ;;
esac

if [[ -z "$SYSTEM_DIR" ]]; then
  echo "No system directory is configured for $DEVICE in $ENV_FILE." >&2
  exit 1
fi
if [[ ! -f "$SYSTEM_DIR/lib/libc++.so" ]]; then
  echo "The $DEVICE system directory does not contain lib/libc++.so: $SYSTEM_DIR" >&2
  exit 1
fi

ANDROID_NDK=${ANDROID_NDK:-/home/jax/Android/Sdk/ndk/r21e}
if [[ ! -f "$ANDROID_NDK/build/cmake/android.toolchain.cmake" ]]; then
  echo "Set ANDROID_NDK to an Android NDK containing android.toolchain.cmake." >&2
  exit 1
fi

if [[ $GENERATE_HIDL -eq 1 ]]; then
  "$ROOT_DIR/scripts/generate-hidl-headers.sh"
fi
cmake -S "$ROOT_DIR/system" -B "$ROOT_DIR/build/android-$DEVICE" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=armeabi-v7a \
  -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
  -DANDROID_STL=c++_shared \
  -DSYSTEM_DIR="$SYSTEM_DIR" \
  -DHIDL_GENERATED_DIR="$ROOT_DIR/build/generated/hidl-android10" \
  "${DEVICE_OPTIONS[@]}" \
  "$@"

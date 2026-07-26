#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DEVICE=${1:-nokia-2780-flip}
if [[ $# -gt 0 ]]; then
  shift
fi
ENV_FILE="$ROOT_DIR/.env"

if [[ ! -f "$ENV_FILE" ]]; then
  echo "Missing $ENV_FILE. Copy .env.example and set SYSTEM_DIR." >&2
  exit 1
fi

set -a
source "$ENV_FILE"
set +a

: "${SYSTEM_DIR:?SYSTEM_DIR must be set in .env}"
if [[ ! -f "$SYSTEM_DIR/lib/libc++.so" ]]; then
  echo "SYSTEM_DIR does not contain lib/libc++.so: $SYSTEM_DIR" >&2
  exit 1
fi

case "$DEVICE" in
  nokia-2780-flip) ;;
  nokia-8110-4g)
    echo "Nokia 8110 4G is reserved but has no build target yet." >&2
    exit 1
    ;;
  *)
    echo "Unknown device: $DEVICE" >&2
    exit 1
    ;;
esac

ANDROID_NDK=${ANDROID_NDK:-/home/jax/Android/Sdk/ndk/r21e}
if [[ ! -f "$ANDROID_NDK/build/cmake/android.toolchain.cmake" ]]; then
  echo "Set ANDROID_NDK to an Android NDK containing android.toolchain.cmake." >&2
  exit 1
fi

"$ROOT_DIR/scripts/generate-hidl-headers.sh"
cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build/android-$DEVICE" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=armeabi-v7a \
  -DANDROID_PLATFORM=android-29 \
  -DSYSTEM_DIR="$SYSTEM_DIR" \
  -DHIDL_GENERATED_DIR="$ROOT_DIR/build/generated/hidl-android10" \
  "$@"

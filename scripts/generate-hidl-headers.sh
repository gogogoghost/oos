#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
HOST_BUILD_DIR="$ROOT_DIR/build/host-hidl-gen"
OUTPUT_DIR="$ROOT_DIR/build/generated/hidl-android10"

"$ROOT_DIR/scripts/apply-third-party-patches.sh"
cmake -S "$ROOT_DIR/tools/kaios-hidl-gen" -B "$HOST_BUILD_DIR"
cmake --build "$HOST_BUILD_DIR" --target hidl-gen -j2

HIDL_GEN="$HOST_BUILD_DIR/hidl-gen"
HARDWARE_ROOT="$ROOT_DIR/third_party/aosp-hardware-interfaces"
HIDL_ROOT="$ROOT_DIR/third_party/aosp-system-libhidl/transport"
mkdir -p "$OUTPUT_DIR"

for PACKAGE in \
  android.hidl.base@1.0 \
  android.hidl.manager@1.0 \
  android.hardware.graphics.common@1.0 \
  android.hardware.graphics.common@1.1 \
  android.hardware.graphics.common@1.2 \
  android.hardware.graphics.bufferqueue@1.0 \
  android.hardware.graphics.bufferqueue@2.0 \
  android.hardware.graphics.composer@2.1 \
  android.hardware.graphics.composer@2.2 \
  android.hardware.graphics.composer@2.3 \
  android.hardware.media@1.0 \
  android.hardware.power@1.0; do
  "$HIDL_GEN" -o "$OUTPUT_DIR" -L c++-headers \
    -r "android.hardware:$HARDWARE_ROOT" \
    -r "android.hidl:$HIDL_ROOT" "$PACKAGE"
done

#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build/android-nokia-8110-4g"
BINARY="$BUILD_DIR/bin/tests/nokia-8110-4g/oos_probe_nokia_8110_graphics"
REMOTE_BINARY=/data/local/tmp/oos-probe-8110-graphics
ADB=${ADB:-adb}

if [[ $("$ADB" shell id | tr -d '\r') != *"uid=0(root)"* ]]; then
  echo "The Nokia 8110 ADB shell must already run as root." >&2
  exit 1
fi

"$ROOT_DIR/scripts/configure-android.sh" nokia-8110-4g
cmake --build "$BUILD_DIR" --target oos_probe_nokia_8110_graphics \
  -j"$(nproc)"
"$ADB" push "$BINARY" "$REMOTE_BINARY"
"$ADB" shell "chmod 0755 $REMOTE_BINARY"

echo "The probe does not stop B2G or modify the framebuffer."
"$ADB" shell "$REMOTE_BINARY"

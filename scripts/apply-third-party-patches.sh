#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
GECKO_DIR="$ROOT_DIR/third_party/gecko-b2g"
PATCH_FILE="$ROOT_DIR/patches/gecko-b2g-hwc2-destructor.patch"

if rg -q '~Device\(\)' "$GECKO_DIR/widget/gonk/hwchal/android_10/HWC2.h"; then
  exit 0
fi

git -C "$GECKO_DIR" apply "$PATCH_FILE"

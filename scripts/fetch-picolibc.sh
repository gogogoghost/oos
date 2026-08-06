#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT_DIR/third_party/versions.env"
DESTINATION="$ROOT_DIR/third_party/picolibc"
if [[ ! -d "$DESTINATION/.git" ]]; then
  git clone "$PICOLIBC_REPOSITORY" "$DESTINATION"
fi
if [[ "$(git -C "$DESTINATION" rev-parse HEAD)" != "$PICOLIBC_COMMIT" ]]; then
  git -C "$DESTINATION" fetch --depth 1 origin "$PICOLIBC_COMMIT"
  git -C "$DESTINATION" checkout --detach "$PICOLIBC_COMMIT"
fi
test "$(git -C "$DESTINATION" rev-parse HEAD)" = "$PICOLIBC_COMMIT"
PATCH_FILE="$ROOT_DIR/system/patches/picolibc-wasm32-machine.patch"
if [[ ! -f "$DESTINATION/newlib/libc/machine/wasm32/meson.build" ]]; then
  patch --batch --silent -d "$DESTINATION" -p1 <"$PATCH_FILE"
fi

#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT_DIR/third_party/versions.env"
if [[ -f "$ROOT_DIR/.env" ]]; then
  set -a
  source "$ROOT_DIR/.env"
  set +a
fi
BUILD_CONTAINER=${OOS_WAMR_DISTROBOX:-}
if [[ -n "$BUILD_CONTAINER" && "${OOS_IN_DISTROBOX:-0}" != 1 ]]; then
  exec distrobox enter "$BUILD_CONTAINER" -- env OOS_IN_DISTROBOX=1 \
    "$0"
fi

"$ROOT_DIR/scripts/fetch-picolibc.sh"
BUILD_DIR="$ROOT_DIR/build/wasm-picolibc"
INSTALL_DIR="$BUILD_DIR/install"
STAMP_FILE="$BUILD_DIR/build-inputs.sha256"
BUILD_INPUTS=$(printf '%s\n' "$PICOLIBC_COMMIT" \
  "$(sha256sum "$ROOT_DIR/system/patches/picolibc-wasm32-machine.patch")" \
  "$(sha256sum "$ROOT_DIR/sdk/c/toolchains/wasm32-picolibc.ini")" |
  sha256sum | cut -d' ' -f1)
if [[ -f "$INSTALL_DIR/lib/libc.a" && -f "$INSTALL_DIR/lib/libm.a" &&
      -f "$STAMP_FILE" && "$(<"$STAMP_FILE")" == "$BUILD_INPUTS" ]]; then
  exit 0
fi
SETUP_MODE=()
[[ -f "$BUILD_DIR/build.ninja" ]] && SETUP_MODE=(--wipe)
meson setup "${SETUP_MODE[@]}" "$BUILD_DIR" "$ROOT_DIR/third_party/picolibc" \
  --cross-file "$ROOT_DIR/sdk/c/toolchains/wasm32-picolibc.ini" \
  --prefix "$INSTALL_DIR" --buildtype release \
  -Dmultilib=false -Dpicocrt=false -Dsemihost=false -Dtests=false \
  -Dposix-io=false -Dnewlib-global-errno=true \
  -Dthread-local-storage=false -Datomic-ungetc=true \
  -Dformat-default=double
meson compile -C "$BUILD_DIR" -j "${OOS_BUILD_JOBS:-24}"
meson install -C "$BUILD_DIR"
test -f "$INSTALL_DIR/lib/libc.a"
test -f "$INSTALL_DIR/lib/libm.a"
printf '%s\n' "$BUILD_INPUTS" >"$STAMP_FILE"

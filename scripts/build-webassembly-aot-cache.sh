#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DEVICE=${1:-}
[[ -n $DEVICE ]] && shift

usage() {
  echo "usage: $0 DEVICE WASM_FILE..." >&2
}

case "$DEVICE" in
  nokia-2780-flip|nokia-8110-4g) ;;
  *) usage; exit 2 ;;
esac
[[ $# -gt 0 ]] || { usage; exit 2; }

if [[ -f "$ROOT_DIR/.env" ]]; then
  set -a
  source "$ROOT_DIR/.env"
  set +a
fi
source "$ROOT_DIR/third_party/versions.env"
source "$ROOT_DIR/system/config/wpe/devices/$DEVICE.env"

UNSAFE_FAST=${OOS_WAMR_WEB_UNSAFE_FAST:-OFF}
case "$UNSAFE_FAST" in
  ON|1|true|TRUE) UNSAFE_FAST=ON ;;
  OFF|0|false|FALSE) UNSAFE_FAST=OFF ;;
  *)
    echo "OOS_WAMR_WEB_UNSAFE_FAST must be ON or OFF" >&2
    exit 2
    ;;
esac

AOT_ARCH=${OOS_WAMR_AOT_ARCH:-armv7}
AOT_ABI=${OOS_WAMR_AOT_ABI:-eabi}
AOT_TRIPLE=${OOS_WAMR_AOT_TRIPLE:-}
AOT_CPU=${OOS_WAMR_AOT_CPU:-generic}
AOT_CPU_FEATURES=${OOS_WAMR_AOT_CPU_FEATURES:-}
WAMR_RELEASE=${WAMR_VERSION#WAMR-}
if [[ $AOT_CPU == generic && $UNSAFE_FAST == OFF ]]; then
  NAMESPACE="wamr-$WAMR_RELEASE/armv7a-nosimd-bounds-checks"
elif [[ $UNSAFE_FAST == ON ]]; then
  NAMESPACE="wamr-$WAMR_RELEASE/armv7-$AOT_CPU-nosimd-no-bounds"
else
  NAMESPACE="wamr-$WAMR_RELEASE/armv7-$AOT_CPU-nosimd-bounds-checks"
fi
OUTPUT_DIR=${OOS_WAMR_WEB_AOT_CACHE_OUTPUT:-"$ROOT_DIR/build/web-aot-cache"}
DESTINATION="$OUTPUT_DIR/$NAMESPACE"

"$ROOT_DIR/scripts/build-wamrc.sh"
WAMRC="$ROOT_DIR/build/host-wamrc/wamrc"
WAMRC_ARGS=(--disable-simd --invoke-c-api-import)
if [[ -n $AOT_TRIPLE ]]; then
  WAMRC_ARGS+=(--target="$AOT_TRIPLE" --cpu="$AOT_CPU")
elif [[ $AOT_CPU == generic ]]; then
  # This is the ARMv7 triple accepted by both existing device runtimes.
  WAMRC_ARGS+=(--target=armv7a-pc-linux-eabi)
else
  WAMRC_ARGS+=(--target="$AOT_ARCH" --cpu="$AOT_CPU")
  WAMRC_ARGS+=(--target-abi="$AOT_ABI")
fi
if [[ $AOT_CPU != generic && -n $AOT_CPU_FEATURES ]]; then
  WAMRC_ARGS+=(--cpu-features="$AOT_CPU_FEATURES")
fi
if [[ $UNSAFE_FAST == ON ]]; then
  WAMRC_ARGS+=(
    --bounds-checks=0
    --stack-bounds-checks=0
    --disable-aux-stack-check)
else
  WAMRC_ARGS+=(--bounds-checks=1 --stack-bounds-checks=1)
fi

mkdir -p "$DESTINATION"
temporary=
cleanup() {
  [[ -z $temporary ]] || rm -f "$temporary"
}
trap cleanup EXIT
for wasm_file in "$@"; do
  [[ -f $wasm_file ]] || {
    echo "Wasm input does not exist: $wasm_file" >&2
    exit 1
  }
  checksum=$(sha256sum "$wasm_file" | awk '{print $1}')
  output="$DESTINATION/$checksum.aot"
  temporary="$output.tmp.$$"
  rm -f "$temporary"
  args=("${WAMRC_ARGS[@]}" -o "$temporary" "$wasm_file")
  if [[ -n ${OOS_WAMR_DISTROBOX:-} ]]; then
    distrobox enter "$OOS_WAMR_DISTROBOX" -- "$WAMRC" "${args[@]}"
  else
    "$WAMRC" "${args[@]}"
  fi
  mv -f "$temporary" "$output"
  temporary=
  echo "AOT cache: $output"
done

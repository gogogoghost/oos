#!/usr/bin/env bash

set -euo pipefail
umask 022

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT_DIR/scripts/lib/package-common.sh"

DEVICE=nokia-2780-flip
OUTPUT_DIR=
RES_VERSION=1.0.0
CREATE_TGZ=0

usage() {
  echo "usage: $0 [--device DEVICE] [--output DIR] [--res-version VERSION] [--tgz]" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --device)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      DEVICE=$2
      shift 2
      ;;
    --output)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      OUTPUT_DIR=$2
      shift 2
      ;;
    --res-version)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      RES_VERSION=$2
      shift 2
      ;;
    --tgz)
      CREATE_TGZ=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage
      exit 2
      ;;
  esac
done

[[ "$DEVICE" == nokia-2780-flip || "$DEVICE" == nokia-8110-4g ]] ||
  package_die "Scaffold packaging is not implemented for $DEVICE"
[[ "$RES_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-.+][0-9A-Za-z.-]+)?$ ]] ||
  package_die "Invalid res version: $RES_VERSION"

OUTPUT_DIR=${OUTPUT_DIR:-$ROOT_DIR/dist/$DEVICE/oos}
if [[ -e "$OUTPUT_DIR" ]]; then
  package_die "Output already exists: $OUTPUT_DIR"
fi

TEMPLATE_DIR="$ROOT_DIR/system/packaging/scaffold"
package_require_directory "$TEMPLATE_DIR"

STAGING_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/oos-scaffold.XXXXXX")
trap 'rm -rf "$STAGING_ROOT"' EXIT
STAGING="$STAGING_ROOT/oos"
mkdir -p "$STAGING"

install -m 0755 "$TEMPLATE_DIR/bootstrap.sh" "$STAGING/bootstrap.sh"
install -m 0755 "$TEMPLATE_DIR/activate-res.sh" "$STAGING/activate-res.sh"
install -m 0755 "$TEMPLATE_DIR/init.sh" "$STAGING/init.sh"
install -m 0755 "$TEMPLATE_DIR/deinit.sh" "$STAGING/deinit.sh"
install -m 0755 "$TEMPLATE_DIR/start.sh" "$STAGING/start.sh"
install -m 0755 "$TEMPLATE_DIR/gc-res.sh" "$STAGING/gc-res.sh"

mkdir -p "$STAGING/rootfs/system" \
  "$STAGING/rootfs/vendor" \
  "$STAGING/rootfs/apex/com.android.runtime" \
  "$STAGING/rootfs/dev" \
  "$STAGING/rootfs/proc" \
  "$STAGING/rootfs/sys" \
  "$STAGING/rootfs/data" \
  "$STAGING/rootfs/opt/oos" \
  "$STAGING/rootfs/home/jax/project/oos/build/wpe-sysroot"

# WPE 2.52.5 currently embeds this build prefix for injected-bundle lookup.
# Keep the compatibility entirely inside the chroot until WPE is rebuilt with
# /opt/oos as its install prefix.
ln -s /opt/oos \
  "$STAGING/rootfs/home/jax/project/oos/build/wpe-sysroot/$DEVICE"

case "$DEVICE" in
  nokia-2780-flip)
    HWC_SERVICE=vendor.hwcomposer-2-1
    REMOVABLE_STORAGE=/storage/sdcard1
    ;;
  nokia-8110-4g)
    HWC_SERVICE=
    REMOVABLE_STORAGE=/storage/sdcard
    ;;
esac

printf '%s\n' \
  "OOS_SCAFFOLD_FORMAT=2" \
  "OOS_DEVICE=$DEVICE" \
  "OOS_HWC_SERVICE=$HWC_SERVICE" \
  "OOS_INTERNAL_STORAGE=/storage/emulated/0" \
  "OOS_REMOVABLE_STORAGE_CANDIDATES=$REMOVABLE_STORAGE" \
  > "$STAGING/bootstrap.conf"

printf '%s\n' \
  "format=2" \
  "type=oos-scaffold" \
  "device=$DEVICE" \
  "default_res=$RES_VERSION" \
  "git_commit=$(git -C "$ROOT_DIR" rev-parse HEAD)" \
  > "$STAGING/manifest.env"

ln -s "res-$RES_VERSION" "$STAGING/res"
package_write_checksums "$STAGING"

mkdir -p "$(dirname "$OUTPUT_DIR")"
mv "$STAGING" "$OUTPUT_DIR"

if [[ "$CREATE_TGZ" -eq 1 ]]; then
  ARCHIVE="$(dirname "$OUTPUT_DIR")/oos-scaffold-$DEVICE.tgz"
  package_create_tgz "$OUTPUT_DIR" "$ARCHIVE"
  echo "Created $ARCHIVE"
fi
echo "Created $OUTPUT_DIR"

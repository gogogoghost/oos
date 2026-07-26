#!/usr/bin/env bash

set -euo pipefail
umask 022

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT_DIR/scripts/lib/package-common.sh"

DEVICE=nokia-2780-flip
OUTPUT_DIR=
VERSION=
CREATE_TGZ=0
ACTIVATE=0

usage() {
  echo "usage: $0 VERSION [--device DEVICE] [--output OOS_DIR] [--tgz] [--activate]" >&2
}

if [[ $# -gt 0 && "$1" != -* ]]; then
  VERSION=$1
  shift
fi
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
    --tgz)
      CREATE_TGZ=1
      shift
      ;;
    --activate)
      ACTIVATE=1
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

[[ "$DEVICE" == nokia-2780-flip ]] ||
  package_die "Res packaging is not implemented for $DEVICE"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-.+][0-9A-Za-z.-]+)?$ ]] ||
  package_die "Invalid or missing res version: $VERSION"

OUTPUT_DIR=${OUTPUT_DIR:-$ROOT_DIR/dist/$DEVICE/oos}
package_require_directory "$OUTPUT_DIR"

RES_NAME="res-$VERSION"
DESTINATION="$OUTPUT_DIR/$RES_NAME"
[[ ! -e "$DESTINATION" ]] || package_die "Res output already exists: $DESTINATION"

OOS_BINARY="$ROOT_DIR/build/android-$DEVICE/bin/oos"
WPE_SYSROOT="$ROOT_DIR/build/wpe-sysroot/$DEVICE"
package_require_file "$OOS_BINARY"
package_require_directory "$WPE_SYSROOT/lib"
package_require_directory "$WPE_SYSROOT/libexec"

if [[ -f "$ROOT_DIR/.env" ]]; then
  set -a
  source "$ROOT_DIR/.env"
  set +a
fi
WPE_NDK=${WPE_NDK:-/home/jax/Android/Sdk/ndk/magisk}
CXX_RUNTIME="$WPE_NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/arm-linux-androideabi/libc++_shared.so"
if [[ ! -f "$CXX_RUNTIME" ]]; then
  CXX_RUNTIME="$WPE_NDK/sources/cxx-stl/llvm-libc++/libs/armeabi-v7a/libc++_shared.so"
fi
package_require_file "$CXX_RUNTIME"
SYSTEM_DIR=${SYSTEM_DIR:-}
package_require_directory "$SYSTEM_DIR/lib"

STAGING_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/oos-res.XXXXXX")
cleanup() {
  rm -rf "$STAGING_ROOT"
  if [[ -n "${ACTIVATE_TMP:-}" ]]; then
    rm -f "$ACTIVATE_TMP"
  fi
}
trap cleanup EXIT
STAGING="$STAGING_ROOT/$RES_NAME"
mkdir -p "$STAGING/bin" "$STAGING/lib" "$STAGING/libexec" \
  "$STAGING/share" "$STAGING/etc"

install -m 0755 "$OOS_BINARY" "$STAGING/bin/oos"
install -m 0755 "$CXX_RUNTIME" "$STAGING/lib/libc++_shared.so"

# Copy target shared objects and their symlinks while excluding headers,
# archives, pkg-config metadata, and host-side tools from the 1.3 GB sysroot.
rsync -a --prune-empty-dirs \
  --include='*/' --include='*.so' --include='*.so.*' --exclude='*' \
  "$WPE_SYSROOT/lib/" "$STAGING/lib/"
rsync -a "$WPE_SYSROOT/libexec/" "$STAGING/libexec/"

for runtime_share in fontconfig glib-2.0 gstreamer gstreamer-1.0 \
  gst-plugins-base icu licenses locale wpe-webkit-2.0 xml; do
  if [[ -d "$WPE_SYSROOT/share/$runtime_share" ]]; then
    rsync -a "$WPE_SYSROOT/share/$runtime_share" "$STAGING/share/"
  fi
done
if [[ -d "$WPE_SYSROOT/etc" ]]; then
  rsync -a "$WPE_SYSROOT/etc/" "$STAGING/etc/"
fi

package_verify_elf_dependencies "$STAGING" "$SYSTEM_DIR"

printf '%s\n' \
  "format=1" \
  "type=oos-res" \
  "version=$VERSION" \
  "device=$DEVICE" \
  "abi=armeabi-v7a" \
  "android_api=29" \
  "runtime_prefix=/opt/oos" \
  "git_commit=$(git -C "$ROOT_DIR" rev-parse HEAD)" \
  > "$STAGING/manifest.env"
package_write_checksums "$STAGING"

mv "$STAGING" "$DESTINATION"

if [[ "$ACTIVATE" -eq 1 ]]; then
  ACTIVATE_TMP="$OUTPUT_DIR/.res.$$.new"
  ln -s "$RES_NAME" "$ACTIVATE_TMP"
  mv -Tf "$ACTIVATE_TMP" "$OUTPUT_DIR/res"
  ACTIVATE_TMP=
  echo "Activated $RES_NAME"
fi

if [[ "$CREATE_TGZ" -eq 1 ]]; then
  ARCHIVE="$(dirname "$OUTPUT_DIR")/oos-res-$DEVICE-$VERSION.tgz"
  package_create_tgz "$DESTINATION" "$ARCHIVE"
  echo "Created $ARCHIVE"
fi
echo "Created $DESTINATION"

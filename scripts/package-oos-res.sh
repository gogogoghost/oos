#!/usr/bin/env bash

set -euo pipefail
umask 022

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT_DIR/scripts/lib/package-common.sh"
source "$ROOT_DIR/third_party/versions.env"

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
    --device) DEVICE=${2:-}; shift 2 ;;
    --output) OUTPUT_DIR=${2:-}; shift 2 ;;
    --tgz) CREATE_TGZ=1; shift ;;
    --activate) ACTIVATE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done

[[ "$DEVICE" == nokia-2780-flip || "$DEVICE" == nokia-8110-4g ]] ||
  package_die "Res packaging is not implemented for $DEVICE"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-.+][0-9A-Za-z.-]+)?$ ]] ||
  package_die "Invalid or missing res version: $VERSION"

if [[ -f "$ROOT_DIR/.env" ]]; then
  set -a
  source "$ROOT_DIR/.env"
  set +a
fi
case "$DEVICE" in
  nokia-2780-flip)
    SYSTEM_DIR=${NOKIA_2780_SYSTEM_DIR:-}
    ANDROID_API=29
    ;;
  nokia-8110-4g)
    SYSTEM_DIR=${NOKIA_8110_SYSTEM_DIR:-}
    ANDROID_API=23
    ;;
esac
package_require_directory "$SYSTEM_DIR/lib"

OUTPUT_DIR=${OUTPUT_DIR:-$ROOT_DIR/dist/$DEVICE/oos}
package_require_directory "$OUTPUT_DIR"
RES_NAME="res-$VERSION"
DESTINATION="$OUTPUT_DIR/$RES_NAME"
[[ ! -e "$DESTINATION" ]] || package_die "Res output already exists: $DESTINATION"

OOS_BINARY="$ROOT_DIR/build/android-$DEVICE/bin/oos"
DEVICE_RUNTIME_LIBRARY=
if [[ "$DEVICE" == nokia-8110-4g ]]; then
  DEVICE_RUNTIME_LIBRARY="$ROOT_DIR/build/android-$DEVICE/devices/$DEVICE/liboos-android23-buffer.so"
fi
WAMR_LICENSE="$ROOT_DIR/third_party/wasm-micro-runtime/LICENSE"
LVGL_LICENSE="$ROOT_DIR/third_party/lvgl/LICENCE.txt"
FONT_AWESOME_LICENSE="$ROOT_DIR/third_party/lvgl/scripts/built_in_font/font_license/FontAwesome5/LICENSE.txt"
IMGUI_LICENSE="$ROOT_DIR/third_party/imgui/LICENSE.txt"
SYSTEM_FONT="$ROOT_DIR/system/assets/fonts/ui-proportional.otf"
SYSTEM_FONT_LICENSE="$ROOT_DIR/system/assets/fonts/LICENSE.txt"

package_require_file "$OOS_BINARY"
if [[ -n "$DEVICE_RUNTIME_LIBRARY" ]]; then
  package_require_file "$DEVICE_RUNTIME_LIBRARY"
fi
package_require_file "$WAMR_LICENSE"
package_require_file "$LVGL_LICENSE"
package_require_file "$FONT_AWESOME_LICENSE"
package_require_file "$IMGUI_LICENSE"
package_require_file "$SYSTEM_FONT"
package_require_file "$SYSTEM_FONT_LICENSE"

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
  "$STAGING/share/fonts" \
  "$STAGING/share/licenses/oos"

install -m 0755 "$OOS_BINARY" "$STAGING/bin/oos"
if [[ -n "$DEVICE_RUNTIME_LIBRARY" ]]; then
  install -m 0755 "$DEVICE_RUNTIME_LIBRARY" "$STAGING/lib/"
fi
install -m 0644 "$WAMR_LICENSE" \
  "$STAGING/share/licenses/oos/WAMR.txt"
install -m 0644 "$LVGL_LICENSE" \
  "$STAGING/share/licenses/oos/LVGL.txt"
install -m 0644 "$FONT_AWESOME_LICENSE" \
  "$STAGING/share/licenses/oos/FontAwesome5.txt"
install -m 0644 "$IMGUI_LICENSE" \
  "$STAGING/share/licenses/oos/DearImGui.txt"
install -m 0644 "$SYSTEM_FONT" \
  "$STAGING/share/fonts/ui-proportional.otf"
install -m 0644 "$SYSTEM_FONT_LICENSE" \
  "$STAGING/share/licenses/oos/RedHatFonts.txt"

package_verify_elf_dependencies "$STAGING" "$SYSTEM_DIR"

ANDROID_NDK=${ANDROID_NDK:-/home/jax/Android/Sdk/ndk/r21e}
if [[ -n ${OOS_STRIP:-} ]]; then
  STRIP_TOOL=$OOS_STRIP
elif [[ -x "$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" ]]; then
  STRIP_TOOL="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
elif command -v llvm-strip >/dev/null 2>&1; then
  STRIP_TOOL=$(command -v llvm-strip)
else
  package_die "Cannot find llvm-strip; set OOS_STRIP in .env"
fi
"$STRIP_TOOL" --strip-unneeded "$STAGING/bin/oos"
if [[ -n "$DEVICE_RUNTIME_LIBRARY" ]]; then
  "$STRIP_TOOL" --strip-unneeded \
    "$STAGING/lib/$(basename "$DEVICE_RUNTIME_LIBRARY")"
fi

printf '%s\n' \
  "format=2" \
  "type=oos-res" \
  "version=$VERSION" \
  "device=$DEVICE" \
  "abi=armeabi-v7a" \
  "android_api=$ANDROID_API" \
  "native_app_runtime=${WAMR_VERSION}" \
  "native_app_execution=interpreter,aot" \
  "native_app_interface=oos-wit-0.1.0-core" \
  "system_font=ui-proportional.otf" \
  "system_icons=font-awesome-5-free" \
  "system_ui=lvgl-${LVGL_VERSION#v}" \
  "debug_ui_backend=dear-imgui-${IMGUI_VERSION#v}" \
  "runtime_prefix=/opt/oos" \
  "git_commit=$(git -C "$ROOT_DIR" rev-parse HEAD)" \
  >"$STAGING/manifest.env"
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

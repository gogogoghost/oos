#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT_DIR/third_party/versions.env"
ROOTFS=${OOS_LOCAL_ROOTFS:-"$ROOT_DIR/build/local-root"}
OOS_PREFIX="$ROOTFS/opt/oos"

required=(
  "$ROOT_DIR/build/local/bin/oos"
  "$ROOT_DIR/apps/launcher/dist/application.zip"
  "$ROOT_DIR/apps/settings/dist/application.zip"
  "$ROOT_DIR/apps/systemui/dist/application.zip"
  "$ROOT_DIR/system/devices/local/config/keymap.conf"
  "$ROOT_DIR/third_party/wasm-micro-runtime/LICENSE"
  "$ROOT_DIR/third_party/quickjs/LICENSE"
  "$ROOT_DIR/third_party/stb/LICENSE"
  "$ROOT_DIR/third_party/clay/LICENSE.md"
  "$ROOT_DIR/third_party/lvgl/LICENCE.txt"
  "$ROOT_DIR/third_party/lvgl/scripts/built_in_font/font_license/FontAwesome5/LICENSE.txt"
  "$ROOT_DIR/third_party/miniaudio/LICENSE"
  "$ROOT_DIR/third_party/ffmpeg/COPYING.LGPLv2.1"
  "$ROOT_DIR/third_party/sonivox/NOTICE"
  "$ROOT_DIR/third_party/fluidlite/LICENSE"
  "$ROOT_DIR/system/licenses/TinyMidiLoader-Zlib.txt"
  "$ROOT_DIR/third_party/generaluser-gs/documentation/LICENSE.txt"
  "$ROOT_DIR/build/local/third_party/fluidlite/libfluidlite.so"
  "$ROOT_DIR/build/media-codecs/local/install/lib/libavformat.so")
for path in "${required[@]}"; do
  [[ -f "$path" ]] || {
    echo "Missing local rootfs input: $path" >&2
    exit 1
  }
done

mkdir -p "$OOS_PREFIX/bin" "$OOS_PREFIX/lib" \
  "$OOS_PREFIX/etc" \
  "$OOS_PREFIX/share/licenses/oos" \
  "$ROOTFS/dev" "$ROOTFS/etc" "$ROOTFS/proc" "$ROOTFS/run" \
  "$ROOTFS/tmp" "$ROOTFS/usr" "$ROOTFS/data"
for link in bin lib lib64 sbin; do
  if [[ ! -e "$ROOTFS/$link" && ! -L "$ROOTFS/$link" ]]; then
    ln -s "usr/$link" "$ROOTFS/$link"
  fi
done
install -m 0755 "$ROOT_DIR/build/local/bin/oos" "$OOS_PREFIX/bin/oos"
rm -f "$OOS_PREFIX/share/fonts/ui-proportional.otf" \
  "$OOS_PREFIX/share/licenses/oos/RedHatFonts.txt"
rmdir "$OOS_PREFIX/share/fonts" 2>/dev/null || true
for app_id in cc.jaxy.oos.launcher cc.jaxy.oos.settings cc.jaxy.oos.systemui; do
  mkdir -p "$OOS_PREFIX/packages/$app_id"
  install -m 0644 "$ROOT_DIR/apps/${app_id##*.}/dist/application.zip" \
    "$OOS_PREFIX/packages/$app_id/application.zip"
done
install -m 0644 "$ROOT_DIR/system/devices/local/config/keymap.conf" \
  "$OOS_PREFIX/etc/local-keymap.conf"
install -m 0644 "$ROOT_DIR/third_party/wasm-micro-runtime/LICENSE" \
  "$OOS_PREFIX/share/licenses/oos/WAMR.txt"
install -m 0644 "$ROOT_DIR/third_party/quickjs/LICENSE" \
  "$OOS_PREFIX/share/licenses/oos/QuickJS.txt"
install -m 0644 "$ROOT_DIR/third_party/stb/LICENSE" \
  "$OOS_PREFIX/share/licenses/oos/stb.txt"
install -m 0644 "$ROOT_DIR/third_party/clay/LICENSE.md" \
  "$OOS_PREFIX/share/licenses/oos/clay.txt"
install -m 0644 "$ROOT_DIR/third_party/lvgl/LICENCE.txt" \
  "$OOS_PREFIX/share/licenses/oos/LVGL.txt"
install -m 0644 \
  "$ROOT_DIR/third_party/lvgl/scripts/built_in_font/font_license/FontAwesome5/LICENSE.txt" \
  "$OOS_PREFIX/share/licenses/oos/FontAwesome5.txt"
install -m 0644 "$ROOT_DIR/third_party/miniaudio/LICENSE" \
  "$OOS_PREFIX/share/licenses/oos/miniaudio.txt"
install -m 0644 "$ROOT_DIR/third_party/ffmpeg/COPYING.LGPLv2.1" \
  "$OOS_PREFIX/share/licenses/oos/FFmpeg-LGPL-2.1.txt"
install -m 0644 "$ROOT_DIR/third_party/sonivox/NOTICE" \
  "$OOS_PREFIX/share/licenses/oos/Sonivox-NOTICE.txt"
install -m 0644 "$ROOT_DIR/third_party/fluidlite/LICENSE" \
  "$OOS_PREFIX/share/licenses/oos/FluidLite-LGPL-2.1.txt"
install -m 0644 "$ROOT_DIR/system/licenses/TinyMidiLoader-Zlib.txt" \
  "$OOS_PREFIX/share/licenses/oos/TinyMidiLoader-Zlib.txt"
install -m 0644 \
  "$ROOT_DIR/third_party/generaluser-gs/documentation/LICENSE.txt" \
  "$OOS_PREFIX/share/licenses/oos/GeneralUser-GS.txt"
rm -f "$OOS_PREFIX/lib/libfluidlite.so"*
cp -a "$ROOT_DIR/build/local/third_party/fluidlite/libfluidlite.so"* \
  "$OOS_PREFIX/lib/"
for library in avformat avcodec swresample avutil; do
  rm -f "$OOS_PREFIX/lib/lib${library}.so"*
  cp -a "$ROOT_DIR/build/media-codecs/local/install/lib/lib${library}.so"* \
    "$OOS_PREFIX/lib/"
done

printf '%s\n' \
  "format=2" \
  "device=local" \
  "prefix=/opt/oos" \
  "app_runtimes=quickjs-${QUICKJS_VERSION},wamr-${WAMR_VERSION}" \
  "media_miniaudio=${MINIAUDIO_VERSION}" \
  "media_ffmpeg=${FFMPEG_VERSION}" \
  "media_sonivox=${SONIVOX_VERSION}" \
  "media_fluidlite=${FLUIDLITE_VERSION}" \
  "media_generaluser_gs=${GENERALUSER_GS_VERSION}" \
  "system_ui=lvgl-wasm-package" \
  "system_icons=font-awesome-5-free" \
  "system_font=host-system-font" \
  "git_commit=$(git -C "$ROOT_DIR" rev-parse HEAD)" \
  >"$OOS_PREFIX/rootfs-manifest.env"
echo "Local OOS rootfs ready at $ROOTFS"

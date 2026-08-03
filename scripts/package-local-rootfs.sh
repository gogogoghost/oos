#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT_DIR/third_party/versions.env"
ROOTFS=${OOS_LOCAL_ROOTFS:-"$ROOT_DIR/build/local-root"}
OOS_PREFIX="$ROOTFS/opt/oos"

required=(
  "$ROOT_DIR/build/local/bin/oos"
  "$ROOT_DIR/system/devices/local/config/keymap.conf"
  "$ROOT_DIR/third_party/wasm-micro-runtime/LICENSE"
  "$ROOT_DIR/third_party/lvgl/LICENCE.txt"
  "$ROOT_DIR/third_party/lvgl/scripts/built_in_font/font_license/FontAwesome5/LICENSE.txt"
  "$ROOT_DIR/third_party/imgui/LICENSE.txt"
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
for launcher_id in org.orangeos.launcher cc.jaxy.oos.launcher; do
  rm -f "$OOS_PREFIX/packages/$launcher_id/application.zip"
  rmdir "$OOS_PREFIX/packages/$launcher_id" 2>/dev/null || true
done
rmdir "$OOS_PREFIX/packages" 2>/dev/null || true
install -m 0644 "$ROOT_DIR/system/devices/local/config/keymap.conf" \
  "$OOS_PREFIX/etc/local-keymap.conf"
install -m 0644 "$ROOT_DIR/third_party/wasm-micro-runtime/LICENSE" \
  "$OOS_PREFIX/share/licenses/oos/WAMR.txt"
install -m 0644 "$ROOT_DIR/third_party/lvgl/LICENCE.txt" \
  "$OOS_PREFIX/share/licenses/oos/LVGL.txt"
install -m 0644 \
  "$ROOT_DIR/third_party/lvgl/scripts/built_in_font/font_license/FontAwesome5/LICENSE.txt" \
  "$OOS_PREFIX/share/licenses/oos/FontAwesome5.txt"
install -m 0644 "$ROOT_DIR/third_party/imgui/LICENSE.txt" \
  "$OOS_PREFIX/share/licenses/oos/DearImGui.txt"
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
  "native_app_runtime=wamr" \
  "media_miniaudio=${MINIAUDIO_VERSION}" \
  "media_ffmpeg=${FFMPEG_VERSION}" \
  "media_sonivox=${SONIVOX_VERSION}" \
  "media_fluidlite=${FLUIDLITE_VERSION}" \
  "media_generaluser_gs=${GENERALUSER_GS_VERSION}" \
  "system_ui=lvgl" \
  "system_icons=font-awesome-5-free" \
  "debug_ui_backend=dear-imgui" \
  "system_font=host-system-font" \
  "git_commit=$(git -C "$ROOT_DIR" rev-parse HEAD)" \
  >"$OOS_PREFIX/rootfs-manifest.env"
echo "Local OOS rootfs ready at $ROOTFS"

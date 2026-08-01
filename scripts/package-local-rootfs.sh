#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ROOTFS=${OOS_LOCAL_ROOTFS:-"$ROOT_DIR/build/local-root"}
OOS_PREFIX="$ROOTFS/opt/oos"

required=(
  "$ROOT_DIR/build/local/bin/oos"
  "$ROOT_DIR/system/devices/local/config/keymap.conf"
  "$ROOT_DIR/system/assets/fonts/ui-proportional.otf"
  "$ROOT_DIR/system/assets/fonts/LICENSE.txt"
  "$ROOT_DIR/third_party/wasm-micro-runtime/LICENSE"
  "$ROOT_DIR/third_party/lvgl/LICENCE.txt"
  "$ROOT_DIR/third_party/imgui/LICENSE.txt")
for path in "${required[@]}"; do
  [[ -f "$path" ]] || {
    echo "Missing local rootfs input: $path" >&2
    exit 1
  }
done

mkdir -p "$OOS_PREFIX/bin" \
  "$OOS_PREFIX/etc" \
  "$OOS_PREFIX/share/fonts" "$OOS_PREFIX/share/licenses/oos" \
  "$ROOTFS/dev" "$ROOTFS/etc" "$ROOTFS/proc" "$ROOTFS/run" \
  "$ROOTFS/tmp" "$ROOTFS/usr" "$ROOTFS/data"
for link in bin lib lib64 sbin; do
  if [[ ! -e "$ROOTFS/$link" && ! -L "$ROOTFS/$link" ]]; then
    ln -s "usr/$link" "$ROOTFS/$link"
  fi
done
install -m 0755 "$ROOT_DIR/build/local/bin/oos" "$OOS_PREFIX/bin/oos"
rm -f "$OOS_PREFIX/packages/org.orangeos.launcher/application.zip"
rmdir "$OOS_PREFIX/packages/org.orangeos.launcher" \
  "$OOS_PREFIX/packages" 2>/dev/null || true
install -m 0644 "$ROOT_DIR/system/devices/local/config/keymap.conf" \
  "$OOS_PREFIX/etc/local-keymap.conf"
install -m 0644 "$ROOT_DIR/system/assets/fonts/ui-proportional.otf" \
  "$OOS_PREFIX/share/fonts/ui-proportional.otf"
install -m 0644 "$ROOT_DIR/system/assets/fonts/LICENSE.txt" \
  "$OOS_PREFIX/share/licenses/oos/RedHatFonts.txt"
install -m 0644 "$ROOT_DIR/third_party/wasm-micro-runtime/LICENSE" \
  "$OOS_PREFIX/share/licenses/oos/WAMR.txt"
install -m 0644 "$ROOT_DIR/third_party/lvgl/LICENCE.txt" \
  "$OOS_PREFIX/share/licenses/oos/LVGL.txt"
install -m 0644 "$ROOT_DIR/third_party/imgui/LICENSE.txt" \
  "$OOS_PREFIX/share/licenses/oos/DearImGui.txt"

printf '%s\n' \
  "format=2" \
  "device=local" \
  "prefix=/opt/oos" \
  "native_app_runtime=wamr" \
  "system_ui=lvgl" \
  "debug_ui_backend=dear-imgui" \
  "system_font=ui-proportional.otf" \
  "git_commit=$(git -C "$ROOT_DIR" rev-parse HEAD)" \
  >"$OOS_PREFIX/rootfs-manifest.env"
echo "Local OOS rootfs ready at $ROOTFS"

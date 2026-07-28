#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ROOTFS=${OOS_LOCAL_ROOTFS:-"$ROOT_DIR/build/wpe-sysroot/local-root"}
OOS_PREFIX="$ROOTFS/opt/oos"
GSTREAMER_REGISTRY="$OOS_PREFIX/share/gstreamer-1.0/registry.bin"

required=(
  "$ROOT_DIR/build/local/bin/oos"
  "$ROOT_DIR/build/local/bin/oos-web-local"
  "$ROOT_DIR/build/native-apps/launcher.wasm"
  "$ROOT_DIR/apps/web-launcher/dist/index.html"
  "$ROOT_DIR/system/devices/local/config/keymap.conf"
  "$ROOT_DIR/system/assets/boot/nokia-2780-flip/boot-splash.png"
  "$OOS_PREFIX/lib/libWPEWebKit-2.0.so"
  "$OOS_PREFIX/libexec/wpe-webkit-2.0/WPEWebProcess")
for path in "${required[@]}"; do
  [[ -f "$path" ]] || {
    echo "Missing local rootfs input: $path" >&2
    exit 1
  }
done

mkdir -p "$OOS_PREFIX/bin" "$OOS_PREFIX/apps/web-launcher" \
  "$OOS_PREFIX/etc" "$OOS_PREFIX/share/oos" \
  "$(dirname "$GSTREAMER_REGISTRY")" \
  "$ROOTFS/dev" "$ROOTFS/etc" "$ROOTFS/proc" "$ROOTFS/run" \
  "$ROOTFS/tmp" "$ROOTFS/usr"
for link in bin lib lib64 sbin; do
  if [[ ! -e "$ROOTFS/$link" && ! -L "$ROOTFS/$link" ]]; then
    ln -s "usr/$link" "$ROOTFS/$link"
  fi
done
install -m 0755 "$ROOT_DIR/build/local/bin/oos" "$OOS_PREFIX/bin/oos"
install -m 0755 "$ROOT_DIR/build/local/bin/oos-web-local" \
  "$OOS_PREFIX/bin/oos-web-local"
install -m 0644 "$ROOT_DIR/build/native-apps/launcher.wasm" \
  "$OOS_PREFIX/apps/launcher.wasm"
install -m 0644 "$ROOT_DIR/apps/web-launcher/dist/index.html" \
  "$OOS_PREFIX/apps/web-launcher/index.html"
install -m 0644 "$ROOT_DIR/system/devices/local/config/keymap.conf" \
  "$OOS_PREFIX/etc/local-keymap.conf"
install -m 0644 \
  "$ROOT_DIR/system/assets/boot/nokia-2780-flip/boot-splash.png" \
  "$OOS_PREFIX/share/oos/boot-splash.png"

command -v gst-inspect-1.0 >/dev/null || {
  echo "gst-inspect-1.0 is required to package the local rootfs" >&2
  exit 1
}
GST_PLUGIN_PATH='' \
GST_PLUGIN_SYSTEM_PATH=$(pkg-config --variable=pluginsdir gstreamer-1.0) \
GST_REGISTRY="$GSTREAMER_REGISTRY" \
GST_REGISTRY_UPDATE=yes \
  gst-inspect-1.0 --version >/dev/null
[[ -s "$GSTREAMER_REGISTRY" ]] || {
  echo "Failed to generate local GStreamer registry" >&2
  exit 1
}
chmod 0644 "$GSTREAMER_REGISTRY"

printf '%s\n' \
  "format=1" \
  "device=local" \
  "prefix=/opt/oos" \
  "gstreamer=$(gst-inspect-1.0 --version | sed -n '1s/.*version //p')" \
  "git_commit=$(git -C "$ROOT_DIR" rev-parse HEAD)" \
  >"$OOS_PREFIX/rootfs-manifest.env"
echo "Local OOS rootfs ready at $ROOTFS"

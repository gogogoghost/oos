#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ROOTFS=${OOS_LOCAL_ROOTFS:-"$ROOT_DIR/build/local-root"}
OOS_PREFIX="$ROOTFS/opt/oos"

required=(
  "$ROOT_DIR/build/local/bin/oos"
  "$ROOT_DIR/build/native-apps/launcher.wasm"
  "$ROOT_DIR/system/devices/local/config/keymap.conf"
  "$ROOT_DIR/system/assets/fonts/ui-proportional.otf"
  "$ROOT_DIR/system/assets/fonts/LICENSE.txt")
for path in "${required[@]}"; do
  [[ -f "$path" ]] || {
    echo "Missing local rootfs input: $path" >&2
    exit 1
  }
done

mkdir -p "$OOS_PREFIX/bin" \
  "$OOS_PREFIX/packages/org.orangeos.launcher" \
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
"$ROOT_DIR/scripts/package-oos-wasm-app.sh" \
  --manifest "$ROOT_DIR/apps/launcher/oos-manifest.json" \
  --wasm "$ROOT_DIR/build/native-apps/launcher.wasm" \
  --output "$OOS_PREFIX/packages/org.orangeos.launcher/application.zip"
install -m 0644 "$ROOT_DIR/system/devices/local/config/keymap.conf" \
  "$OOS_PREFIX/etc/local-keymap.conf"
install -m 0644 "$ROOT_DIR/system/assets/fonts/ui-proportional.otf" \
  "$OOS_PREFIX/share/fonts/ui-proportional.otf"
install -m 0644 "$ROOT_DIR/system/assets/fonts/LICENSE.txt" \
  "$OOS_PREFIX/share/licenses/oos/RedHatFonts.txt"

printf '%s\n' \
  "format=2" \
  "device=local" \
  "prefix=/opt/oos" \
  "native_app_runtime=wamr" \
  "system_font=ui-proportional.otf" \
  "git_commit=$(git -C "$ROOT_DIR" rev-parse HEAD)" \
  >"$OOS_PREFIX/rootfs-manifest.env"
echo "Local OOS rootfs ready at $ROOTFS"

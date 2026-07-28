#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ROOTFS=${OOS_LOCAL_ROOTFS:-"$ROOT_DIR/build/wpe-sysroot/local-root"}
MODE=${1:-native}
[[ $# -eq 0 ]] || shift

case "$MODE" in
  native)
    PROGRAM=/opt/oos/bin/oos
    DEFAULT_ARGUMENT=/opt/oos/apps/launcher.wasm
    ;;
  web)
    PROGRAM=/opt/oos/bin/oos-web-local
    DEFAULT_ARGUMENT=/opt/oos/apps/web-launcher/index.html
    ;;
  *)
    echo "usage: $0 [native|web] [APP]" >&2
    exit 2
    ;;
esac
[[ $# -le 1 ]] || { echo "usage: $0 [native|web] [APP]" >&2; exit 2; }
ARGUMENT=${1:-$DEFAULT_ARGUMENT}

[[ -x "$ROOTFS$PROGRAM" ]] || {
  echo "Local rootfs is not packaged: $ROOTFS" >&2
  exit 1
}

common_env=(
  --setenv HOME /tmp
  --setenv LD_LIBRARY_PATH /opt/oos/lib
  --setenv OOS_BOOT_SPLASH /opt/oos/share/oos/boot-splash.png
  --setenv OOS_LAUNCHER_MODULE /opt/oos/apps/launcher.wasm
  --setenv OOS_LOCAL_KEYMAP /opt/oos/etc/local-keymap.conf
  --setenv GST_REGISTRY /opt/oos/share/gstreamer-1.0/registry.bin
  --setenv GST_REGISTRY_UPDATE no
  --setenv LIBGL_ALWAYS_SOFTWARE 1
  --setenv GALLIUM_DRIVER llvmpipe
  --setenv MESA_LOADER_DRIVER_OVERRIDE llvmpipe
  --setenv __EGL_VENDOR_LIBRARY_FILENAMES /usr/share/glvnd/egl_vendor.d/50_mesa.json
  --setenv __GLX_VENDOR_LIBRARY_NAME mesa)
for name in DISPLAY WAYLAND_DISPLAY XDG_RUNTIME_DIR; do
  if [[ -n ${!name:-} ]]; then
    common_env+=(--setenv "$name" "${!name}")
  fi
done

if command -v bwrap >/dev/null 2>&1; then
  namespace_args=(
    --die-with-parent --unshare-user --uid 0 --gid 0
    --ro-bind "$ROOTFS" /
    --ro-bind /usr /usr
    --ro-bind /etc /etc
    --proc /proc
    --dev /dev
    --tmpfs /run
    --tmpfs /tmp)
  if [[ -n ${XDG_RUNTIME_DIR:-} && -n ${WAYLAND_DISPLAY:-} &&
        -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]]; then
    namespace_args+=(--dir /run/user --dir "$XDG_RUNTIME_DIR"
      --ro-bind "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY"
      "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY")
    common_env+=(--setenv SDL_VIDEODRIVER "${SDL_VIDEODRIVER:-wayland}")
  fi
  if [[ -d /tmp/.X11-unix ]]; then
    namespace_args+=(--ro-bind /tmp/.X11-unix /tmp/.X11-unix)
  fi
  if [[ -n ${XAUTHORITY:-} && -f ${XAUTHORITY:-} ]]; then
    namespace_args+=(--ro-bind "$XAUTHORITY" "$XAUTHORITY")
    common_env+=(--setenv XAUTHORITY "$XAUTHORITY")
  fi
  exec bwrap "${namespace_args[@]}" "${common_env[@]}" -- \
    "$PROGRAM" "$ARGUMENT"
fi

if command -v proot >/dev/null 2>&1; then
  export HOME=/tmp
  export LD_LIBRARY_PATH=/opt/oos/lib
  export OOS_BOOT_SPLASH=/opt/oos/share/oos/boot-splash.png
  export OOS_LAUNCHER_MODULE=/opt/oos/apps/launcher.wasm
  export OOS_LOCAL_KEYMAP=/opt/oos/etc/local-keymap.conf
  export GST_REGISTRY=/opt/oos/share/gstreamer-1.0/registry.bin
  export GST_REGISTRY_UPDATE=no
  export LIBGL_ALWAYS_SOFTWARE=1
  export GALLIUM_DRIVER=llvmpipe
  export MESA_LOADER_DRIVER_OVERRIDE=llvmpipe
  export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
  export __GLX_VENDOR_LIBRARY_NAME=mesa
  exec proot -0 -r "$ROOTFS" -b /usr -b /etc -b /proc \
    "$PROGRAM" "$ARGUMENT"
fi

echo "Neither bubblewrap nor proot is installed." >&2
exit 1

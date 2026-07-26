#!/system/bin/sh

set -eu

OOS_HOME=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$OOS_HOME/bootstrap.sh"

acquire_bootstrap_lock start
trap 'release_bootstrap_lock' 0

if [ -f "$OOS_PID_FILE" ]; then
  existing_pid=$(cat "$OOS_PID_FILE" 2>/dev/null || true)
  if [ -n "$existing_pid" ] && kill -0 "$existing_pid" 2>/dev/null; then
    echo "OOS is already running with pid $existing_pid" >&2
    exit 1
  fi
  rm -f "$OOS_PID_FILE"
fi

# init.sh runs as a child of this shell and therefore shares the mount
# namespace used by the chroot below.
"$OOS_HOME/init.sh"
resolve_res

setprop ctl.stop b2g 2>/dev/null || true
setprop ctl.stop b2gkillerd 2>/dev/null || true
echo 0 > /sys/class/leds/lcd-backlight/brightness 2>/dev/null || true
echo 0 > /sys/class/leds/sublcd-backlight/brightness 2>/dev/null || true
echo 4 > /sys/class/graphics/fb1/blank 2>/dev/null || true

if [ -n "${OOS_HWC_SERVICE:-}" ]; then
  setprop ctl.restart "$OOS_HWC_SERVICE"
  wait_for_service "$OOS_HWC_SERVICE" 50
fi

release_bootstrap_lock
trap - 0

export OOS_RES_VERSION=$(sed -n 's/^version=//p' "$OOS_RES_DIR/manifest.env")
export HOME=/data
export TMPDIR=/data/tmp
export XDG_CACHE_HOME=/data/cache
export XDG_DATA_HOME=/data/share
export LD_LIBRARY_PATH=/opt/oos/lib:/apex/com.android.runtime/lib:/system/lib:/vendor/lib
export WEBKIT_EXEC_PATH=/opt/oos/libexec/wpe-webkit-2.0
export WPE_BACKEND=/opt/oos/lib/libWPEBackend-android.so
export GIO_EXTRA_MODULES=/opt/oos/lib/gio/modules
export GST_PLUGIN_SYSTEM_PATH=/opt/oos/lib/gstreamer-1.0
export FONTCONFIG_FILE=/opt/oos/etc/fonts/fonts.conf
export SSL_CERT_FILE=/opt/oos/etc/ssl/certs/ca-certificates.crt

mkdir -p "$OOS_PERSIST_DIR/tmp" "$OOS_PERSIST_DIR/cache" \
  "$OOS_PERSIST_DIR/share"

chroot "$OOS_ROOTFS" /system/bin/sh -c \
  'if [ "$#" -eq 0 ] && [ -f /opt/oos/apps/launcher.aot ]; then
     exec /opt/oos/bin/oos /opt/oos/apps/launcher.aot
   fi
   exec /opt/oos/bin/oos "$@"' oos "$@" &
oos_pid=$!
echo "$oos_pid" > "$OOS_PID_FILE"

forward_signal() {
  kill -TERM "$oos_pid" 2>/dev/null || true
}
trap forward_signal HUP INT TERM

set +e
wait "$oos_pid"
result=$?
set -e
if [ "$(cat "$OOS_PID_FILE" 2>/dev/null || true)" = "$oos_pid" ]; then
  rm -f "$OOS_PID_FILE"
fi
trap - HUP INT TERM
exit "$result"

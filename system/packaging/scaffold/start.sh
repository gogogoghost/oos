#!/system/bin/sh

set -eu

OOS_HOME=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$OOS_HOME/bootstrap.sh"

acquire_bootstrap_lock start
trap 'release_bootstrap_lock' 0

if [ -f "$OOS_PID_FILE" ]; then
  existing_pid=$(cat "$OOS_PID_FILE" 2>/dev/null || true)
  if oos_pid_running "$existing_pid"; then
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
export HOME=/data/users/0/system
export TMPDIR=/data/tmp
export XDG_CACHE_HOME=/data/cache/system
export XDG_DATA_HOME=/data/users/0/system/share
# The host owns the Android display HAL. System libraries must win here so a
# WPE dependency (notably libpng) cannot interpose on a HAL dependency. The
# WPE child switches back to its private runtime-first path before exec.
export LD_LIBRARY_PATH=/system/lib:/vendor/lib:/apex/com.android.runtime/lib:/opt/oos/lib
export OOS_WPE_LD_LIBRARY_PATH=/opt/oos/lib:/system/lib:/vendor/lib:/apex/com.android.runtime/lib
export WEBKIT_EXEC_PATH=/opt/oos/libexec/wpe-webkit-2.0
export WEBKIT_INJECTED_BUNDLE_PATH=/opt/oos/lib/wpe-webkit-2.0/injected-bundle
export WPE_BACKEND=/opt/oos/lib/libWPEBackend-android.so
export GIO_EXTRA_MODULES=/opt/oos/lib/gio/modules
export GST_PLUGIN_SYSTEM_PATH=/opt/oos/lib/gstreamer-1.0
export FONTCONFIG_FILE=/opt/oos/etc/fonts/fonts.conf
export SSL_CERT_FILE=/opt/oos/etc/ssl/certs/ca-certificates.crt

mkdir -p "$OOS_PERSIST_DIR/tmp" "$OOS_PERSIST_DIR/cache/system" \
  "$OOS_PERSIST_DIR/users/0/system/share"

chroot "$OOS_ROOTFS" /system/bin/sh -c \
  'exec /opt/oos/bin/oos "$@"' oos "$@" &
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

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

if [ -n "${OOS_AUDIO_READY_MODE:-}" ]; then
  wait_for_audio_runtime "$OOS_AUDIO_READY_MODE"
fi

setprop ctl.stop b2g 2>/dev/null || true
setprop ctl.stop b2gkillerd 2>/dev/null || true
echo 0 > /sys/class/leds/lcd-backlight/brightness 2>/dev/null || true
if [ -e /sys/class/leds/sublcd-backlight/brightness ]; then
  echo 0 > /sys/class/leds/sublcd-backlight/brightness
fi
if [ -e /sys/class/graphics/fb1/blank ]; then
  echo 4 > /sys/class/graphics/fb1/blank
fi

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
# The host owns the Android display HAL, so stock system libraries take
# precedence over any future private OOS libraries.
export LD_LIBRARY_PATH=/system/lib:/vendor/lib:/apex/com.android.runtime/lib:/opt/oos/lib

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

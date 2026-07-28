#!/system/bin/sh

set -eu

. "$(CDPATH= cd "$(dirname "$0")" && pwd)/bootstrap.sh"

unmount_if_mounted() {
  target_path=$1
  if ! is_mounted "$target_path"; then
    return 0
  fi
  if umount "$target_path"; then
    return 0
  fi
  if [ "${OOS_FORCE_LAZY_UNMOUNT:-0}" = 1 ]; then
    umount -l "$target_path"
    return 0
  fi
  echo "failed to unmount $target_path" >&2
  return 1
}

stop_oos() {
  if [ ! -f "$OOS_PID_FILE" ]; then
    return 0
  fi
  oos_pid=$(cat "$OOS_PID_FILE" 2>/dev/null || true)
  if oos_pid_running "$oos_pid"; then
    kill -TERM "$oos_pid" 2>/dev/null || true
    attempt=0
    while kill -0 "$oos_pid" 2>/dev/null; do
      attempt=$((attempt + 1))
      if [ "$attempt" -ge 30 ]; then
        kill -KILL "$oos_pid" 2>/dev/null || true
        break
      fi
      sleep 0.1
    done
  fi
  rm -f "$OOS_PID_FILE"
}

acquire_bootstrap_lock deinit
trap 'release_bootstrap_lock' 0

stop_oos
echo 0 > /sys/class/leds/lcd-backlight/brightness 2>/dev/null || true
echo 0 > /sys/class/leds/sublcd-backlight/brightness 2>/dev/null || true
echo 4 > /sys/class/graphics/fb1/blank 2>/dev/null || true

status=0
unmount_if_mounted "$OOS_ROOTFS/data/media/removable" || status=1
unmount_if_mounted "$OOS_ROOTFS/data/media/internal" || status=1
unmount_if_mounted "$OOS_ROOTFS/opt/oos" || status=1
unmount_if_mounted "$OOS_ROOTFS/data" || status=1
unmount_if_mounted "$OOS_ROOTFS/apex/com.android.runtime" || status=1
unmount_if_mounted "$OOS_ROOTFS/vendor" || status=1
unmount_if_mounted "$OOS_ROOTFS/sys" || status=1
unmount_if_mounted "$OOS_ROOTFS/proc" || status=1
unmount_if_mounted "$OOS_ROOTFS/dev" || status=1
unmount_if_mounted "$OOS_ROOTFS/system" || status=1

if [ "$status" -eq 0 ]; then
  rm -f "$OOS_ACTIVE_RES_FILE" "$OOS_STATE_DIR/removable-source"
  echo "OOS rootfs deinitialized"
fi
exit "$status"

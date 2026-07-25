#!/system/bin/sh

set -eu

RUNTIME=/data/local/tmp/oos-wpe
ROOT=/data/local/tmp/oos-wpe-chroot
BUILD_PREFIX=/home/jax/project/oos/build/wpe-sysroot/nokia-2780-flip
LOG="$RUNTIME/hello.log"
PID_FILE="$ROOT/wpe.pid"
# The concurrent fb1 path is diagnostic-only; see devices/nokia-2780-flip/README.md.
OOS_ENABLE_COVER=${OOS_ENABLE_COVER:-0}

unmount_if_mounted() {
  umount -l "$1" 2>/dev/null || true
}

stop() {
  if [ -f "$PID_FILE" ]; then
    kill "$(cat "$PID_FILE")" 2>/dev/null || true
    rm -f "$PID_FILE"
  fi
  unmount_if_mounted "$ROOT/system/lib/libc++_shared.so"
  unmount_if_mounted "$ROOT$BUILD_PREFIX/share"
  unmount_if_mounted "$ROOT$BUILD_PREFIX/libexec"
  unmount_if_mounted "$ROOT$BUILD_PREFIX/lib"
  unmount_if_mounted "$ROOT/apex/com.android.runtime"
  unmount_if_mounted "$ROOT/vendor"
  unmount_if_mounted "$ROOT/data/local/tmp/oos-wpe"
  unmount_if_mounted "$ROOT/sys"
  unmount_if_mounted "$ROOT/proc"
  unmount_if_mounted "$ROOT/dev"
  unmount_if_mounted "$ROOT/system"
}

reset_displays() {
  stop
  # fb0 is owned by HWC. Blank only through its backlight; writing fb0/blank
  # behind the composer leaves its display handle stale on this device.
  echo 0 > /sys/class/leds/lcd-backlight/brightness 2>/dev/null || true
  echo 0 > /sys/class/leds/sublcd-backlight/brightness 2>/dev/null || true
  echo 4 > /sys/class/graphics/fb1/blank 2>/dev/null || true
  setprop ctl.restart vendor.hwcomposer-2-1
  retries=0
  while [ "$(getprop init.svc.vendor.hwcomposer-2-1)" != "running" ]; do
    retries=$((retries + 1))
    if [ "$retries" -ge 5 ]; then
      break
    fi
    sleep 1
  done
  if [ "$(getprop init.svc.vendor.hwcomposer-2-1)" != "running" ]; then
    echo "hardware composer did not restart" >&2
    return 1
  fi
}

start() {
  reset_displays

  # Clean placeholders made by an older per-library APEX bind-mount attempt.
  rm -f "$RUNTIME/lib/libandroidicu.so" "$RUNTIME/lib/libicui18n.so" \
    "$RUNTIME/lib/libicuuc.so"

  mkdir -p "$ROOT/system/lib" "$ROOT/apex/com.android.runtime" \
    "$ROOT/vendor" "$ROOT/data/local/tmp/oos-wpe" "$ROOT/dev" "$ROOT/proc" \
    "$ROOT/sys" "$ROOT$BUILD_PREFIX/lib" "$ROOT$BUILD_PREFIX/libexec" \
    "$ROOT$BUILD_PREFIX/share"
  mount --bind /system "$ROOT/system"
  mount --bind /dev "$ROOT/dev"
  mount -t proc proc "$ROOT/proc"
  mount -t sysfs sysfs "$ROOT/sys"
  mount --bind "$RUNTIME" "$ROOT/data/local/tmp/oos-wpe"
  mount --bind /vendor "$ROOT/vendor"
  mount --bind /apex/com.android.runtime "$ROOT/apex/com.android.runtime"
  # The release WPE build has this prefix compiled into its WebProcess lookup.
  mount --bind "$RUNTIME/lib" "$ROOT$BUILD_PREFIX/lib"
  mount --bind "$RUNTIME/libexec" "$ROOT$BUILD_PREFIX/libexec"
  mount --bind "$RUNTIME/share" "$ROOT$BUILD_PREFIX/share"

  # This mount affects only the chroot view rooted at $ROOT, never /system.
  mount --bind "$RUNTIME/lib/libc++_shared.so" \
    "$ROOT/system/lib/libc++_shared.so"

  echo 255 > /sys/class/leds/lcd-backlight/brightness
  rm -f "$LOG"
  chroot "$ROOT" /system/bin/sh -c "
    export LD_LIBRARY_PATH=$RUNTIME/lib:/apex/com.android.runtime/lib
    export WEBKIT_EXEC_PATH=$RUNTIME/libexec/wpe-webkit-2.0
    export WPE_BACKEND=$RUNTIME/lib/libWPEBackend-android.so
    export OOS_ENABLE_COVER=$OOS_ENABLE_COVER
    exec $RUNTIME/nokia_2780_wpe_hello
  " >"$LOG" 2>&1 &
  echo $! > "$PID_FILE"
  echo "started pid=$(cat "$PID_FILE") log=$LOG"
}

case "${1:-start}" in
  start) start ;;
  stop) stop ;;
  reset) reset_displays ;;
  status)
    [ -f "$PID_FILE" ] && ps -p "$(cat "$PID_FILE")" -o PID,ARGS || true
    cat "$LOG" 2>/dev/null || true
    ;;
  *) echo "usage: $0 {start|stop|status}" >&2; exit 2 ;;
esac

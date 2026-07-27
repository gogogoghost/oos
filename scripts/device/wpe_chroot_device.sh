#!/system/bin/sh

set -eu

RUNTIME=/data/local/tmp/oos-wpe
ROOT=/data/local/tmp/oos-wpe-chroot
CONFIG="$RUNTIME/device.env"
LOG="$RUNTIME/hello.log"
PID_FILE="$RUNTIME/wpe.pid"
HOST_PID_FILE="$RUNTIME/wpe-host.pid"
PRODUCER_PID_FILE="$RUNTIME/wpe-producer.pid"
SURFACE_SOCKET="$RUNTIME/wpe-surface.sock"
TEST_HOLD_MS=${OOS_WPE_TEST_HOLD_MS:-10000}

[ -f "$CONFIG" ] || { echo "missing $CONFIG" >&2; exit 1; }
. "$CONFIG"

unmount_if_mounted() {
  if command -v busybox >/dev/null 2>&1; then
    busybox umount -l "$1" 2>/dev/null || true
  else
    umount -l "$1" 2>/dev/null || true
  fi
}

mount_bind() {
  if command -v busybox >/dev/null 2>&1; then
    busybox mount -o bind "$1" "$2"
  else
    mount -o bind "$1" "$2"
  fi
}

mount_filesystem() {
  if command -v busybox >/dev/null 2>&1; then
    busybox mount -t "$1" "$2" "$3"
  else
    mount -t "$1" "$2" "$3"
  fi
}

stop() {
  echo 0 > /sys/class/leds/lcd-backlight/brightness 2>/dev/null || true
  if [ -f "$PID_FILE" ]; then
    pid=$(cat "$PID_FILE")
    kill "$pid" 2>/dev/null || true
    rm -f "$PID_FILE"
  fi
  for child_pid_file in "$PRODUCER_PID_FILE" "$HOST_PID_FILE"; do
    if [ -f "$child_pid_file" ]; then
      kill "$(cat "$child_pid_file")" 2>/dev/null || true
      rm -f "$child_pid_file"
    fi
  done
  rm -f "$SURFACE_SOCKET"
  unmount_if_mounted "$ROOT$BUILD_PREFIX/share"
  unmount_if_mounted "$ROOT$BUILD_PREFIX/etc"
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

reset_display() {
  stop
  if [ "$DEVICE" = "nokia-8110-4g" ]; then
    # HWC1 close and lazy chroot unmounts complete asynchronously on this ROM.
    sleep 1
  fi
  echo 0 > /sys/class/leds/lcd-backlight/brightness 2>/dev/null || true
  if [ "$DEVICE" = "nokia-2780-flip" ]; then
    echo 0 > /sys/class/leds/sublcd-backlight/brightness 2>/dev/null || true
    echo 4 > /sys/class/graphics/fb1/blank 2>/dev/null || true
    setprop ctl.restart vendor.hwcomposer-2-1
    retries=0
    while [ "$(getprop init.svc.vendor.hwcomposer-2-1)" != "running" ]; do
      retries=$((retries + 1))
      [ "$retries" -lt 5 ] || break
      sleep 1
    done
    [ "$(getprop init.svc.vendor.hwcomposer-2-1)" = "running" ] || {
      echo "hardware composer did not restart" >&2
      return 1
    }
  fi
}

bind_if_present() {
  source_path=$1
  target_path=$2
  if [ -d "$source_path" ]; then
    mkdir -p "$target_path"
    mount_bind "$source_path" "$target_path"
  fi
}

supervise_hello() {
  set +e
  chroot "$ROOT" /system/bin/sh -c "
    export LD_LIBRARY_PATH=/system/lib:/vendor/lib:/apex/com.android.runtime/lib:$RUNTIME/lib
    export OOS_SURFACE_SOCKET=$SURFACE_SOCKET
    exec $RUNTIME/$HELLO_HOST
  " &
  host_pid=$!
  echo "$host_pid" > "$HOST_PID_FILE"
  retries=0
  while [ ! -e "$SURFACE_SOCKET" ] && kill -0 "$host_pid" 2>/dev/null; do
    retries=$((retries + 1))
    [ "$retries" -lt 10 ] || break
    sleep 1
  done
  if [ ! -e "$SURFACE_SOCKET" ]; then
    wait "$host_pid"
    host_result=$?
    rm -f "$HOST_PID_FILE" "$SURFACE_SOCKET" "$PID_FILE"
    return "$host_result"
  fi
  chroot "$ROOT" /system/bin/sh -c "
    export LD_LIBRARY_PATH=$RUNTIME/lib:/system/lib:/vendor/lib:/apex/com.android.runtime/lib
    export WEBKIT_EXEC_PATH=$RUNTIME/libexec/wpe-webkit-2.0
    export WEBKIT_INJECTED_BUNDLE_PATH=$RUNTIME/lib/wpe-webkit-2.0/injected-bundle
    export WPE_BACKEND=$RUNTIME/lib/libWPEBackend-android.so
    export GIO_EXTRA_MODULES=$RUNTIME/lib/gio/modules
    export FONTCONFIG_FILE=$RUNTIME/etc/fonts/fonts.conf
    export SSL_CERT_FILE=$RUNTIME/etc/ssl/certs/ca-certificates.crt
    export OOS_WPE_HELLO_HTML=$RUNTIME/hello.html
    export OOS_WPE_TEST_HOLD_MS=$TEST_HOLD_MS
    export OOS_SURFACE_SOCKET=$SURFACE_SOCKET
    export OOS_SURFACE_WIDTH=240
    export OOS_SURFACE_HEIGHT=320
    exec $RUNTIME/$HELLO_PRODUCER
  " &
  producer_pid=$!
  echo "$producer_pid" > "$PRODUCER_PID_FILE"
  wait "$producer_pid"
  producer_result=$?
  wait "$host_pid"
  host_result=$?
  rm -f "$PRODUCER_PID_FILE" "$HOST_PID_FILE" "$SURFACE_SOCKET" "$PID_FILE"
  [ "$producer_result" -eq 0 ] && [ "$host_result" -eq 0 ]
}

start() {
  mode=${1:-hello}
  reset_display
  setenforce 0 2>/dev/null || true
  setprop ctl.stop b2g 2>/dev/null || true
  setprop ctl.stop b2gkillerd 2>/dev/null || true

  mkdir -p "$ROOT/system" "$ROOT/data/local/tmp/oos-wpe" "$ROOT/dev" \
    "$ROOT/proc" "$ROOT/sys" "$ROOT$BUILD_PREFIX/lib" \
    "$ROOT$BUILD_PREFIX/libexec" "$ROOT$BUILD_PREFIX/share" \
    "$ROOT$BUILD_PREFIX/etc"
  mount_bind /system "$ROOT/system"
  mount_bind /dev "$ROOT/dev"
  mount_filesystem proc proc "$ROOT/proc"
  mount_filesystem sysfs sysfs "$ROOT/sys"
  mount_bind "$RUNTIME" "$ROOT/data/local/tmp/oos-wpe"
  bind_if_present /vendor "$ROOT/vendor"
  bind_if_present /apex/com.android.runtime "$ROOT/apex/com.android.runtime"
  mount_bind "$RUNTIME/lib" "$ROOT$BUILD_PREFIX/lib"
  mount_bind "$RUNTIME/libexec" "$ROOT$BUILD_PREFIX/libexec"
  mount_bind "$RUNTIME/share" "$ROOT$BUILD_PREFIX/share"
  mount_bind "$RUNTIME/etc" "$ROOT$BUILD_PREFIX/etc"
  if [ "$DEVICE" = "nokia-8110-4g" ]; then
    sleep 2
  fi

  if [ "$mode" = "input" ]; then
    program=$INPUT_PROGRAM
    rm -f "$LOG"
    chroot "$ROOT" /system/bin/sh -c "
      export LD_LIBRARY_PATH=$RUNTIME/lib:/system/lib:/vendor/lib:/apex/com.android.runtime/lib
      export WEBKIT_EXEC_PATH=$RUNTIME/libexec/wpe-webkit-2.0
      export WEBKIT_INJECTED_BUNDLE_PATH=$RUNTIME/lib/wpe-webkit-2.0/injected-bundle
      export WPE_BACKEND=$RUNTIME/lib/libWPEBackend-android.so
      export GIO_EXTRA_MODULES=$RUNTIME/lib/gio/modules
      export FONTCONFIG_FILE=$RUNTIME/etc/fonts/fonts.conf
      export SSL_CERT_FILE=$RUNTIME/etc/ssl/certs/ca-certificates.crt
      exec $RUNTIME/$program
    " >"$LOG" 2>&1 &
    echo $! > "$PID_FILE"
    return
  fi

  rm -f "$LOG"
  rm -f "$SURFACE_SOCKET"
  if command -v busybox >/dev/null 2>&1; then
    busybox nohup "$0" supervise-hello >"$LOG" 2>&1 </dev/null &
  else
    nohup "$0" supervise-hello >"$LOG" 2>&1 </dev/null &
  fi
  echo $! > "$PID_FILE"
  sleep 1
  if ! kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
    cat "$LOG" >&2 || true
    rm -f "$PID_FILE"
    return 1
  fi
  echo "started device=$DEVICE pid=$(cat "$PID_FILE") log=$LOG"
}

case "${1:-start}" in
  start) start hello ;;
  supervise-hello) supervise_hello ;;
  input-test) start input ;;
  stop) stop ;;
  reset) reset_display ;;
  status)
    if [ -f "$PID_FILE" ]; then
      ps -p "$(cat "$PID_FILE")" -o PID,ARGS 2>/dev/null || true
    fi
    cat "$LOG" 2>/dev/null || true
    ;;
  *) echo "usage: $0 {start|input-test|stop|reset|status}" >&2; exit 2 ;;
esac

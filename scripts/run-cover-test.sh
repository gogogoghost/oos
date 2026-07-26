#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build/android-nokia-2780-flip/bin/tests/nokia-2780-flip"
REMOTE_DIR=/data/local/tmp/oos-display
REMOTE_PID="$REMOTE_DIR/cover.pid"
ADB=${ADB:-adb}

ACTION=${1:-secondary}

stop_cover() {
  "$ADB" shell "su -c 'if [ -f $REMOTE_PID ]; then kill \$(cat $REMOTE_PID) 2>/dev/null || true; rm -f $REMOTE_PID; fi; echo 0 >/sys/class/leds/sublcd-backlight/brightness 2>/dev/null || true; echo 4 >/sys/class/graphics/fb1/blank 2>/dev/null || true'"
}

case "$ACTION" in
  secondary)
    PROGRAM=oos_test_nokia_2780_cover_secondary
    ;;
  green)
    PROGRAM=oos_test_nokia_2780_cover_green
    ;;
  stop)
    stop_cover
    exit 0
    ;;
  status)
    "$ADB" shell "su -c 'if [ -f $REMOTE_PID ]; then ps -p \$(cat $REMOTE_PID) -o PID,ARGS; fi; cat $REMOTE_DIR/cover.log 2>/dev/null || true'"
    exit 0
    ;;
  *)
    echo "usage: $0 {secondary|green|stop|status}" >&2
    exit 2
    ;;
esac

LOCAL_BINARY="$BUILD_DIR/$PROGRAM"
if [[ ! -x "$LOCAL_BINARY" ]]; then
  echo "Missing cover test binary: $LOCAL_BINARY" >&2
  exit 1
fi

"$ADB" shell "su -c 'setprop ctl.stop b2g; setprop ctl.stop b2gkillerd; if [ -x /data/local/tmp/oos-wpe/wpe_chroot_device.sh ]; then /data/local/tmp/oos-wpe/wpe_chroot_device.sh stop; fi; mkdir -p $REMOTE_DIR; chmod 0777 $REMOTE_DIR'"
"$ADB" push "$LOCAL_BINARY" "$REMOTE_DIR/$PROGRAM" >/dev/null
stop_cover
"$ADB" shell "su -c 'chmod 0755 $REMOTE_DIR/$PROGRAM; nohup $REMOTE_DIR/$PROGRAM >$REMOTE_DIR/cover.log 2>&1 </dev/null & echo \$! >$REMOTE_PID'"
for attempt in {1..30}; do
  if "$ADB" shell "su -c 'grep -q \"cover frame presented\" $REMOTE_DIR/cover.log'"; then
    "$ADB" shell "su -c 'pidof $PROGRAM; cat $REMOTE_DIR/cover.log'"
    exit 0
  fi
  sleep 0.1
done

echo "Cover frame was not presented within 3 seconds" >&2
"$ADB" shell "su -c 'cat $REMOTE_DIR/cover.log 2>/dev/null || true'" >&2
stop_cover
exit 1

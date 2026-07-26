#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build/android-nokia-2780-flip"
BINARY="$BUILD_DIR/bin/tests/nokia-2780-flip/oos_test_nokia_2780_network_headless"
REMOTE_BINARY=/data/local/tmp/oos-network-test
ADB=${ADB:-adb}
MODE=${1:-smoke}

case "$MODE" in
  smoke | wifi | bluetooth | deploy) ;;
  *)
    echo "Usage: $0 [smoke|wifi|bluetooth|deploy]" >&2
    exit 2
    ;;
esac

cmake --build "$BUILD_DIR" --target oos_test_nokia_2780_network_headless \
  -j"$(nproc)"
"$ADB" push "$BINARY" "$REMOTE_BINARY"
"$ADB" shell "su -c 'chmod 755 $REMOTE_BINARY; setenforce 0'"

if [[ "$MODE" == deploy ]]; then
  echo "Deployed $REMOTE_BINARY"
  exit 0
fi

run_test() {
  echo "> $REMOTE_BINARY $*"
  "$ADB" shell "su -c '$REMOTE_BINARY $*'"
}

if [[ "$MODE" == smoke || "$MODE" == wifi ]]; then
  run_test wifi status
  run_test wifi networks
  run_test ip status
  run_test wifi scan 3
  run_test wifi cycle
  run_test ip cycle
fi

if [[ "$MODE" == smoke || "$MODE" == bluetooth ]]; then
  run_test bluetooth classic-scan 6
  run_test bluetooth le-scan 8
  bluetooth_state=$(
    "$ADB" shell "su -c 'getprop init.svc.bluetoothd_socket1; getprop bluetooth.isEnabled'" |
      tr -d '\r'
  )
  if [[ "$bluetooth_state" != $'stopped\nfalse' ]]; then
    echo "Bluetooth cleanup failed: $bluetooth_state" >&2
    exit 1
  fi
fi

echo "Network test completed. Wi-Fi and Bluetooth state were restored."

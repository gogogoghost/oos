#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build/android-nokia-2780-flip"
BINARY="$BUILD_DIR/bin/tests/nokia-2780-flip/oos_test_nokia_2780_modem_headless"
REMOTE_BINARY=/data/local/tmp/oos-modem-test
ADB=${ADB:-adb}
MODE=${1:-smoke}

case "$MODE" in
  smoke | deploy) ;;
  *)
    echo "Usage: $0 [smoke|deploy]" >&2
    exit 2
    ;;
esac

cmake --build "$BUILD_DIR" --target oos_test_nokia_2780_modem_headless \
  -j"$(nproc)"
"$ADB" push "$BINARY" "$REMOTE_BINARY"
"$ADB" shell "su -c 'chmod 755 $REMOTE_BINARY; setenforce 0'"

if [[ "$MODE" == deploy ]]; then
  echo "Deployed $REMOTE_BINARY"
  exit 0
fi

echo "> $REMOTE_BINARY status"
status_output=$("$ADB" shell "su -c '$REMOTE_BINARY status'" | tr -d '\r')
printf '%s\n' "$status_output"

for expected in \
  "service_connected=1" \
  "request.getIccCardStatus=none(0)" \
  "request.getBasebandVersion=none(0)" \
  "request.getRadioCapability=none(0)"; do
  if [[ "$status_output" != *"$expected"* ]]; then
    echo "Missing expected modem result: $expected" >&2
    exit 1
  fi
done

service_state=$(
  "$ADB" shell \
    "su -c 'getprop init.svc.vendor.qcrild; getprop vendor.peripheral.modem.state'" |
    tr -d '\r'
)
if [[ "$service_state" != $'running\nONLINE' ]]; then
  echo "Modem service did not remain healthy: $service_state" >&2
  exit 1
fi

echo "Modem smoke test completed. qcrild is running and the modem is online."

#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DEVICE=${1:-nokia-2780-flip}
ADB=${ADB:-adb}

case "$DEVICE" in
  nokia-2780-flip)
    TARGET_PREFIX=oos_test_nokia_2780
    OUTPUT_DEVICE=nokia-2780-flip
    ;;
  nokia-8110-4g)
    TARGET_PREFIX=oos_test_nokia_8110
    OUTPUT_DEVICE=nokia-8110-4g
    ;;
  *)
    echo "Unknown device: $DEVICE" >&2
    exit 2
    ;;
esac

BUILD_DIR="$ROOT_DIR/build/android-$DEVICE"
DEVICE_TARGET="${TARGET_PREFIX}_device_contract"
SERVICE_TARGET="${TARGET_PREFIX}_service_contract"
DEVICE_BINARY="$BUILD_DIR/bin/tests/$OUTPUT_DEVICE/$DEVICE_TARGET"
SERVICE_BINARY="$BUILD_DIR/bin/tests/$OUTPUT_DEVICE/$SERVICE_TARGET"

cmake --build "$BUILD_DIR" --target "$DEVICE_TARGET" "$SERVICE_TARGET" \
  -j"$(nproc)"

REMOTE_DEVICE=/data/local/tmp/oos-device-contract
REMOTE_SERVICE=/data/local/tmp/oos-service-contract
"$ADB" push "$DEVICE_BINARY" "$REMOTE_DEVICE"
"$ADB" push "$SERVICE_BINARY" "$REMOTE_SERVICE"

shell_uid=$("$ADB" shell 'id -u' | tr -d '\r')
if [[ "$shell_uid" == 0 ]]; then
  "$ADB" shell "chmod 755 $REMOTE_DEVICE $REMOTE_SERVICE"
  "$ADB" shell "$REMOTE_DEVICE"
  "$ADB" shell "$REMOTE_SERVICE"
else
  "$ADB" shell "su -c 'chmod 755 $REMOTE_DEVICE $REMOTE_SERVICE'"
  "$ADB" shell "su -c '$REMOTE_DEVICE'"
  "$ADB" shell "su -c '$REMOTE_SERVICE'"
fi

echo "Device and service contracts passed for $DEVICE"

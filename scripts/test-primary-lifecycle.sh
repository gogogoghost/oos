#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ADB=${ADB:-adb}
DURATION=${1:-3}
WPE_LOG=/data/local/tmp/oos-wpe/hello.log

if [[ ! "$DURATION" =~ ^[1-9][0-9]*$ ]]; then
  echo "usage: $0 [positive-seconds]" >&2
  exit 2
fi

"$ROOT_DIR/scripts/run-wpe-chroot.sh" stop
"$ROOT_DIR/scripts/run-wpe-chroot.sh" start

for attempt in {1..30}; do
  if "$ADB" shell "su -c 'grep -q \"primary frame presented\" $WPE_LOG'"; then
    sleep "$DURATION"
    "$ROOT_DIR/scripts/run-wpe-chroot.sh" stop
    echo "primary lifecycle complete"
    exit 0
  fi
  sleep 0.2
done

echo "Primary frame was not presented within 6 seconds" >&2
"$ROOT_DIR/scripts/run-wpe-chroot.sh" status >&2 || true
"$ROOT_DIR/scripts/run-wpe-chroot.sh" stop
exit 1

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

wait_for_primary_frame() {
  local attempt
  for attempt in {1..30}; do
    if "$ADB" shell "su -c 'grep -q \"primary frame presented\" $WPE_LOG'"; then
      return 0
    fi
    sleep 0.2
  done
  echo "Primary frame was not presented within 6 seconds" >&2
  "$ROOT_DIR/scripts/run-wpe-chroot.sh" status >&2 || true
  return 1
}

echo "[1/3] cover: Secondary screen (${DURATION}s)"
"$ROOT_DIR/scripts/run-cover-test.sh" secondary
sleep "$DURATION"

echo "[2/3] primary: WPE Hello World (${DURATION}s)"
"$ROOT_DIR/scripts/run-wpe-chroot.sh" start
wait_for_primary_frame
sleep "$DURATION"

echo "[3/3] cover: Secondary screen (${DURATION}s)"
"$ROOT_DIR/scripts/run-cover-test.sh" secondary
sleep "$DURATION"
"$ROOT_DIR/scripts/run-cover-test.sh" stop

echo "display lifecycle sequence complete"

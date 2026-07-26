#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ADB=${ADB:-adb}
LOG=/data/local/tmp/oos-wpe/hello.log

"$ROOT_DIR/scripts/run-wpe-chroot.sh" input-test

for attempt in {1..40}; do
  if "$ADB" shell "su -c 'grep -q \"key input page ready\" $LOG'"; then
    "$ADB" shell "su -c 'grep -E \"key input device|key input page ready\" $LOG'"
    echo "Key input test is running; press keys and read the code on screen."
    echo "Stop it with: $ROOT_DIR/scripts/run-wpe-chroot.sh stop"
    exit 0
  fi
  sleep 0.2
done

echo "Key input page did not become ready within 8 seconds" >&2
"$ROOT_DIR/scripts/run-wpe-chroot.sh" status >&2 || true
exit 1

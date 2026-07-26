#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ADB=${ADB:-adb}
LOG=/data/local/tmp/oos-wpe/hello.log

cleanup() {
  "$ROOT_DIR/scripts/run-wpe-chroot.sh" stop >/dev/null 2>&1 || true
}
trap cleanup EXIT

"$ROOT_DIR/scripts/run-wpe-chroot.sh" switch-demo

for attempt in {1..100}; do
  if "$ADB" shell "su -c 'grep -q \"single-process demo complete\" $LOG'"; then
    "$ADB" shell "su -c 'grep -E \"single-process|frame ready|black preroll\" $LOG'"
    exit 0
  fi
  if "$ADB" shell "su -c 'grep -q \"single-process demo failed\" $LOG'"; then
    "$ADB" shell "su -c 'cat $LOG'" >&2
    exit 1
  fi
  sleep 0.2
done

echo "Single-process display demo did not finish within 20 seconds" >&2
"$ROOT_DIR/scripts/run-wpe-chroot.sh" status >&2 || true
exit 1

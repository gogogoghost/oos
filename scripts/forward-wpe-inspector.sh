#!/usr/bin/env bash
set -euo pipefail

PORT=${1:-9222}
[[ "$PORT" =~ ^[0-9]+$ ]] && ((PORT > 0 && PORT <= 65535)) || {
  echo "usage: $0 [PORT]" >&2
  exit 2
}

# Recreate the mapping so a disconnected USB transport cannot leave an
# inspector URL backed by a stale adb forwarding socket.
adb forward --remove "tcp:$PORT" >/dev/null 2>&1 || true
adb forward "tcp:$PORT" "tcp:$PORT" >/dev/null
echo "WPE inspector: http://127.0.0.1:$PORT/"

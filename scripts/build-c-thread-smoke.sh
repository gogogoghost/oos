#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUTPUT="$ROOT_DIR/build/native-apps/c-thread-smoke.wasm"
"$ROOT_DIR/scripts/build-c-app.sh" "$OUTPUT" \
  "$ROOT_DIR/sdk/c/tests/thread_smoke.c"

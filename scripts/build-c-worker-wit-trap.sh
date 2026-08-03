#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUTPUT="$ROOT_DIR/build/native-apps/c-worker-wit-trap.wasm"
"$ROOT_DIR/scripts/build-c-app.sh" "$OUTPUT" \
  "$ROOT_DIR/sdk/c/tests/worker_wit_trap.c"

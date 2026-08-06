#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUTPUT="$ROOT_DIR/build/native-apps/c-production-libc-smoke.wasm"
"$ROOT_DIR/scripts/build-c-app.sh" "$OUTPUT" \
  "$ROOT_DIR/sdk/c/tests/production_libc_smoke.c"

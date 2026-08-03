#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
INPUT="$ROOT_DIR/build/native-apps/c-thread-smoke.wasm"
OUTPUT="$ROOT_DIR/build/native-apps/c-thread-smoke.aot"

"$ROOT_DIR/scripts/build-c-thread-smoke.sh"
"$ROOT_DIR/scripts/compile-native-app-aot.sh" "$INPUT" "$OUTPUT"

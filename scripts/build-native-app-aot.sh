#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$ROOT_DIR/scripts/build-native-apps.sh"
INPUT="$ROOT_DIR/build/native-apps/egui-demo.wasm"
OUTPUT="$ROOT_DIR/build/native-apps/egui-demo.aot"

if [[ -f "$OUTPUT" && "$OUTPUT" -nt "$INPUT" \
      && "$OUTPUT" -nt "$ROOT_DIR/scripts/build-native-app-aot.sh" \
      && "$OUTPUT" -nt "$ROOT_DIR/third_party/versions.env" ]]; then
  echo "AOT output is current: $OUTPUT"
  exit 0
fi

"$ROOT_DIR/scripts/compile-native-app-aot.sh" "$INPUT" "$OUTPUT"

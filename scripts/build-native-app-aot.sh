#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$ROOT_DIR/scripts/build-native-apps.sh"
INPUT="$ROOT_DIR/build/native-apps/egui-demo.wasm"

for target in armv7a cortex-a7 cortex-a53; do
  output="$ROOT_DIR/build/native-apps/egui-demo.$target.aot"
  if [[ -f "$output" && "$output" -nt "$INPUT" \
        && "$output" -nt "$ROOT_DIR/scripts/build-native-app-aot.sh" \
        && "$output" -nt "$ROOT_DIR/scripts/compile-native-app-aot.sh" \
        && "$output" -nt "$ROOT_DIR/third_party/versions.env" ]]; then
    echo "AOT output is current: $output"
    continue
  fi

  "$ROOT_DIR/scripts/compile-native-app-aot.sh" "$INPUT" "$output"
done

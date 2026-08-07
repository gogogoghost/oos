#!/usr/bin/env bash

set -euo pipefail

APP_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$APP_DIR/../../.." && pwd)

"$APP_DIR/package.sh"
for target in armv7a cortex-a7 cortex-a53; do
  "$ROOT_DIR/scripts/compile-native-app-aot.sh" \
    "$APP_DIR/build/main.wasm" "$APP_DIR/build/main.$target.aot"
done
"$APP_DIR/package.sh"

echo "Built egui AOT variants under $APP_DIR/build"

#!/usr/bin/env bash

set -euo pipefail

APP_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$APP_DIR/../../.." && pwd)

npm ci --prefix "$APP_DIR"
npm run check --prefix "$APP_DIR"
npm run build --prefix "$APP_DIR"
mkdir -p "$APP_DIR/dist"
"$ROOT_DIR/scripts/package-oos-app.sh" \
  --manifest "$APP_DIR/manifest.json" \
  --js "$APP_DIR/build/main.mjs" \
  --output "$APP_DIR/dist/application.zip"

echo "Packaged Solid demo at $APP_DIR/dist/application.zip"

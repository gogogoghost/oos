#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$ROOT_DIR/apps/tests/lvgl-demo/package.sh"
"$ROOT_DIR/apps/tests/clay-demo/package.sh"
"$ROOT_DIR/apps/tests/solid-demo/package.sh"

echo "Built and packaged LVGL, Clay, and Solid framework demos"

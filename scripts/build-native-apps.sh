#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$ROOT_DIR/apps/launcher/package.sh"
"$ROOT_DIR/apps/settings/package.sh"
"$ROOT_DIR/apps/systemui/package.sh"
"$ROOT_DIR/apps/tests/egui-demo/package.sh"
"$ROOT_DIR/apps/tests/wit-smoke/package.sh"
"$ROOT_DIR/scripts/build-framework-demos.sh"

echo "Built and packaged all OOS applications in their own directories"

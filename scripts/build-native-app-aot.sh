#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
exec "$ROOT_DIR/apps/tests/egui-demo/build-aot.sh"

#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
EXPECTED_VERSION=0.60.0
command -v wit-bindgen >/dev/null 2>&1 || {
  echo "Install wit-bindgen-cli $EXPECTED_VERSION to generate the C SDK." >&2
  exit 1
}
[[ $(wit-bindgen --version) == "wit-bindgen-cli $EXPECTED_VERSION" ]] || {
  echo "C SDK generation requires wit-bindgen-cli $EXPECTED_VERSION." >&2
  exit 1
}
mkdir -p "$ROOT_DIR/sdk/c/generated"
for world in app module; do
  ARGS=(c --world "$world" --out-dir "$ROOT_DIR/sdk/c/generated")
  [[ ${1:-} == --check ]] && ARGS+=(--check)
  wit-bindgen "${ARGS[@]}" "$ROOT_DIR/sdk/wit"
done

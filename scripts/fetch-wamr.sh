#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT_DIR/third_party/versions.env"
DESTINATION="$ROOT_DIR/third_party/wasm-micro-runtime"

if [[ ! -d "$DESTINATION/.git" ]]; then
  git clone --filter=blob:none --branch "$WAMR_VERSION" \
    "$WAMR_REPOSITORY" "$DESTINATION"
fi

actual_commit=$(git -C "$DESTINATION" rev-parse HEAD)
if [[ "$actual_commit" != "$WAMR_COMMIT" ]]; then
  echo "WAMR checkout is at $actual_commit, expected $WAMR_COMMIT" >&2
  echo "Move or update $DESTINATION, then rerun this script." >&2
  exit 1
fi

echo "WAMR $WAMR_VERSION is ready at $DESTINATION"

#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT_DIR/third_party/versions.env"
DESTINATION="$ROOT_DIR/third_party/tlsf"
if [[ ! -d "$DESTINATION/.git" ]]; then
  git clone "$TLSF_REPOSITORY" "$DESTINATION"
fi
if [[ "$(git -C "$DESTINATION" rev-parse HEAD)" != "$TLSF_COMMIT" ]]; then
  git -C "$DESTINATION" fetch --depth 1 origin "$TLSF_COMMIT"
  git -C "$DESTINATION" checkout --detach "$TLSF_COMMIT"
fi
test "$(git -C "$DESTINATION" rev-parse HEAD)" = "$TLSF_COMMIT"

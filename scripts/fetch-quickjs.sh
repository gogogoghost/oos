#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT_DIR/third_party/versions.env"
DESTINATION="$ROOT_DIR/third_party/quickjs"

if [[ ! -d "$DESTINATION/.git" ]]; then
  git clone --filter=blob:none --no-checkout \
    "$QUICKJS_REPOSITORY" "$DESTINATION"
  git -C "$DESTINATION" fetch --depth 1 origin "$QUICKJS_COMMIT"
  git -C "$DESTINATION" checkout --detach "$QUICKJS_COMMIT"
fi

actual_commit=$(git -C "$DESTINATION" rev-parse HEAD)
if [[ "$actual_commit" != "$QUICKJS_COMMIT" ]]; then
  echo "QuickJS checkout is at $actual_commit, expected $QUICKJS_COMMIT" >&2
  echo "Move or update $DESTINATION, then rerun this script." >&2
  exit 1
fi

actual_version=$(tr -d '\r\n' <"$DESTINATION/VERSION")
if [[ "$actual_version" != "$QUICKJS_VERSION" ]]; then
  echo "QuickJS VERSION is $actual_version, expected $QUICKJS_VERSION" >&2
  exit 1
fi

echo "QuickJS $QUICKJS_VERSION is ready at $DESTINATION"

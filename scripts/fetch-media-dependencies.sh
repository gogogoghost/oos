#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT_DIR/third_party/versions.env"

fetch_commit() {
  local name=$1
  local repository=$2
  local revision=$3
  local destination="$ROOT_DIR/third_party/$name"

  if [[ ! -d "$destination/.git" ]]; then
    git clone --filter=blob:none --no-checkout "$repository" "$destination"
  fi
  git -C "$destination" fetch --depth 1 origin "$revision"
  git -C "$destination" checkout --detach "$revision"
  local actual
  actual=$(git -C "$destination" rev-parse HEAD)
  [[ "$actual" == "$revision" ]] || {
    echo "$name checkout mismatch: expected $revision, got $actual" >&2
    exit 1
  }
  echo "$name is ready at $destination"
}

fetch_commit miniaudio "$MINIAUDIO_REPOSITORY" "$MINIAUDIO_COMMIT"
fetch_commit ffmpeg "$FFMPEG_REPOSITORY" "$FFMPEG_COMMIT"
fetch_commit sonivox "$SONIVOX_REPOSITORY" "$SONIVOX_COMMIT"
fetch_commit fluidlite "$FLUIDLITE_REPOSITORY" "$FLUIDLITE_COMMIT"
fetch_commit tinysoundfont "$TINYSOUNDFONT_REPOSITORY" "$TINYSOUNDFONT_COMMIT"
fetch_commit generaluser-gs "$GENERALUSER_GS_REPOSITORY" \
  "$GENERALUSER_GS_COMMIT"

FLUIDLITE_PATCH="$ROOT_DIR/system/patches/fluidlite-zero-copy-soundfont.patch"
if git -C "$ROOT_DIR/third_party/fluidlite" apply --check "$FLUIDLITE_PATCH" \
  >/dev/null 2>&1; then
  git -C "$ROOT_DIR/third_party/fluidlite" apply "$FLUIDLITE_PATCH"
elif ! git -C "$ROOT_DIR/third_party/fluidlite" apply --reverse --check \
  "$FLUIDLITE_PATCH" >/dev/null 2>&1; then
  echo "FluidLite zero-copy patch does not match the pinned revision" >&2
  exit 1
fi

printf '%s  %s\n' "$GENERALUSER_GS_SHA256" \
  "$ROOT_DIR/third_party/generaluser-gs/GeneralUser-GS.sf2" | sha256sum -c -

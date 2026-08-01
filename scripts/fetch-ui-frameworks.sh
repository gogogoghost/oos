#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT_DIR/third_party/versions.env"

fetch_checkout() {
  local name=$1
  local repository=$2
  local version=$3
  local commit=$4
  local destination="$ROOT_DIR/third_party/$name"

  if [[ ! -d "$destination/.git" ]]; then
    git clone --filter=blob:none --no-checkout "$repository" "$destination"
  fi
  git -C "$destination" fetch --depth 1 origin "refs/tags/$version"
  git -C "$destination" checkout --detach "$commit"
  local actual
  actual=$(git -C "$destination" rev-parse HEAD)
  [[ "$actual" == "$commit" ]] || {
    echo "$name checkout mismatch: expected $commit, got $actual" >&2
    exit 1
  }
  echo "$name $version is ready at $destination"
}

fetch_checkout lvgl "$LVGL_REPOSITORY" "$LVGL_VERSION" "$LVGL_COMMIT"
fetch_checkout imgui "$IMGUI_REPOSITORY" "$IMGUI_VERSION" "$IMGUI_COMMIT"

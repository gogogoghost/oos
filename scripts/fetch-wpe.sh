#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT_DIR/third_party/versions.env"

MODE=${1:-all}
CACHE_DIR=${OOS_DOWNLOAD_CACHE:-"$ROOT_DIR/build/downloads"}
SOURCE_DIR="$ROOT_DIR/third_party"

case "$MODE" in
  local | android | all) ;;
  *)
    echo "usage: $0 [local|android|all]" >&2
    exit 2
    ;;
esac

download_release() {
  local name=$1
  local version=$2
  local url=$3
  local expected_sha=$4
  local destination=$5
  local archive="$CACHE_DIR/${url##*/}"
  local marker="$destination/.oos-source-version"

  if [[ -d "$destination" ]]; then
    if [[ -f "$marker" ]] &&
       grep -qx "name=$name" "$marker" &&
       grep -qx "version=$version" "$marker" &&
       grep -qx "sha256=$expected_sha" "$marker"; then
      echo "$name $version is ready at $destination"
      return
    fi
    echo "$destination exists but is not the pinned $name $version source." >&2
    echo "Move it aside or remove it explicitly, then rerun this script." >&2
    exit 1
  fi

  mkdir -p "$CACHE_DIR" "$SOURCE_DIR"
  if [[ ! -f "$archive" ]] ||
     [[ $(sha256sum "$archive" | awk '{print $1}') != "$expected_sha" ]]; then
    local partial="$archive.part"
    curl -fL --retry 3 "$url" -o "$partial"
    echo "$expected_sha  $partial" | sha256sum -c -
    mv "$partial" "$archive"
  fi
  echo "$expected_sha  $archive" | sha256sum -c -

  local staging
  staging=$(mktemp -d "$SOURCE_DIR/.${name}.XXXXXX")
  trap 'rm -rf "$staging"' RETURN
  tar -xf "$archive" -C "$staging" --strip-components=1
  printf '%s\n' \
    "name=$name" \
    "version=$version" \
    "sha256=$expected_sha" \
    "source=$url" \
    >"$staging/.oos-source-version"
  mv "$staging" "$destination"
  trap - RETURN
  echo "$name $version is ready at $destination"
}

fetch_android_cerbero() {
  local destination="$SOURCE_DIR/wpe-android-cerbero"
  local created=0
  if [[ ! -d "$destination/.git" ]]; then
    if [[ -e "$destination" ]]; then
      echo "$destination exists but is not a Git checkout." >&2
      exit 1
    fi
    git clone --filter=blob:none "$WPE_ANDROID_CERBERO_REPOSITORY" \
      "$destination"
    created=1
  fi
  local actual_commit
  actual_commit=$(git -C "$destination" rev-parse HEAD)
  if [[ "$actual_commit" != "$WPE_ANDROID_CERBERO_COMMIT" ]]; then
    if [[ $created -eq 0 ]]; then
      echo "WPE Android Cerbero is at $actual_commit, expected $WPE_ANDROID_CERBERO_COMMIT" >&2
      echo "Move or update $destination explicitly, then rerun this script." >&2
      exit 1
    fi
    if ! git -C "$destination" cat-file -e \
      "$WPE_ANDROID_CERBERO_COMMIT^{commit}" 2>/dev/null; then
      git -C "$destination" fetch --depth=1 origin \
        "$WPE_ANDROID_CERBERO_COMMIT"
    fi
    git -C "$destination" switch --detach "$WPE_ANDROID_CERBERO_COMMIT"
    actual_commit=$(git -C "$destination" rev-parse HEAD)
  fi
  if [[ "$actual_commit" != "$WPE_ANDROID_CERBERO_COMMIT" ]]; then
    echo "Failed to pin WPE Android Cerbero at $WPE_ANDROID_CERBERO_COMMIT" >&2
    exit 1
  fi
  echo "WPE Android Cerbero $actual_commit is ready at $destination"
}

if [[ "$MODE" == local || "$MODE" == all ]]; then
  download_release libwpe "$LIBWPE_VERSION" "$LIBWPE_URL" \
    "$LIBWPE_SHA256" "$SOURCE_DIR/libwpe"
  download_release wpebackend-fdo "$WPEBACKEND_FDO_VERSION" \
    "$WPEBACKEND_FDO_URL" "$WPEBACKEND_FDO_SHA256" \
    "$SOURCE_DIR/wpebackend-fdo"
  download_release wpewebkit "$WPEWEBKIT_VERSION" "$WPEWEBKIT_URL" \
    "$WPEWEBKIT_SHA256" "$SOURCE_DIR/wpewebkit"
fi

if [[ "$MODE" == android || "$MODE" == all ]]; then
  fetch_android_cerbero
fi

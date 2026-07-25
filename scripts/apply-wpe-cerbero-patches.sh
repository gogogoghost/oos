#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CERBERO_DIR=${WPE_CERBERO_DIR:-"$ROOT_DIR/third_party/wpe-android-cerbero"}
PATCH_FILES=(
  "$ROOT_DIR/patches/wpe-android-cerbero-kaios-minimal.patch"
  "$ROOT_DIR/patches/wpe-android-cerbero-use-installed-ndk.patch"
)

if [[ ! -d "$CERBERO_DIR/.git" ]]; then
  echo "Missing Cerbero checkout: $CERBERO_DIR" >&2
  exit 1
fi

for patch_file in "${PATCH_FILES[@]}"; do
  if git -C "$CERBERO_DIR" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
    echo "Cerbero patch already applied: $(basename "$patch_file")"
  elif git -C "$CERBERO_DIR" apply --check "$patch_file" >/dev/null 2>&1; then
    git -C "$CERBERO_DIR" apply "$patch_file"
    echo "Applied Cerbero patch: $(basename "$patch_file")"
  elif [[ $(basename "$patch_file") == wpe-android-cerbero-kaios-minimal.patch ]] && \
       rg -q -- '-DENABLE_WPE_LEGACY_API=ON' "$CERBERO_DIR/recipes/wpewebkit.recipe"; then
    echo "Cerbero patch has the current legacy-backend configuration."
  else
    echo "Cerbero checkout does not match $patch_file." >&2
    exit 1
  fi
done

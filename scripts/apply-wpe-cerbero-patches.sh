#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CERBERO_DIR=${WPE_CERBERO_DIR:-"$ROOT_DIR/third_party/wpe-android-cerbero"}
PATCH_FILES=(
  "$ROOT_DIR/patches/wpe-android-cerbero-kaios-minimal.patch"
  "$ROOT_DIR/patches/wpe-android-cerbero-kaios-performance.patch"
  "$ROOT_DIR/patches/wpe-android-cerbero-use-installed-ndk.patch"
)
SOURCE_PATCHES=(
  "$ROOT_DIR/patches/wpewebkit-kaios-gio-unix.patch:recipes/wpewebkit/0002-KaiOS-Android-use-GioUnix.patch"
  "$ROOT_DIR/patches/wpewebkit-kaios-android-armv7-jit.patch:recipes/wpewebkit/0003-KaiOS-Android-ARMv7-JIT.patch"
)

if [[ ! -d "$CERBERO_DIR/.git" ]]; then
  echo "Missing Cerbero checkout: $CERBERO_DIR" >&2
  exit 1
fi

for patch_mapping in "${SOURCE_PATCHES[@]}"; do
  source_patch=${patch_mapping%%:*}
  destination_patch="$CERBERO_DIR/${patch_mapping#*:}"
  if [[ ! -f "$destination_patch" ]] || ! cmp -s "$source_patch" "$destination_patch"; then
    install -m 0644 "$source_patch" "$destination_patch"
    echo "Installed WebKit source patch: $(basename "$destination_patch")"
  fi
done

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

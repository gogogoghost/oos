#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CERBERO_DIR=${WPE_CERBERO_DIR:-"$ROOT_DIR/third_party/wpe-android-cerbero"}
PATCH_FILES=(
  "$ROOT_DIR/system/patches/wpe-android-cerbero-kaios-minimal.patch"
  "$ROOT_DIR/system/patches/wpe-android-cerbero-kaios-performance.patch"
  "$ROOT_DIR/system/patches/wpe-android-cerbero-use-installed-ndk.patch"
  "$ROOT_DIR/system/patches/wpe-android-cerbero-android23-buffer.patch"
  "$ROOT_DIR/system/patches/wpe-android-cerbero-android23-tasn1.patch"
)
SOURCE_PATCHES=(
  "$ROOT_DIR/system/patches/wpewebkit-kaios-gio-unix.patch:recipes/wpewebkit/0002-KaiOS-Android-use-GioUnix.patch"
  "$ROOT_DIR/system/patches/wpewebkit-kaios-android-armv7-jit.patch:recipes/wpewebkit/0003-KaiOS-Android-ARMv7-JIT.patch"
  "$ROOT_DIR/system/patches/wpewebkit-kaios-android23-buffer.patch:recipes/wpewebkit/0004-KaiOS-Android23-legacy-buffer.patch"
  "$ROOT_DIR/system/patches/wpebackend-android-gralloc0.patch:recipes/wpebackend-android/0001-OOS-RGB565-and-Android23-buffer.patch"
  "$ROOT_DIR/system/patches/libtasn1-android-opaque-file.patch:recipes/libtasn1/0002-Android-opaque-FILE.patch"
)

if [[ ! -d "$CERBERO_DIR/.git" ]]; then
  echo "Missing Cerbero checkout: $CERBERO_DIR" >&2
  exit 1
fi

for patch_mapping in "${SOURCE_PATCHES[@]}"; do
  source_patch=${patch_mapping%%:*}
  destination_patch="$CERBERO_DIR/${patch_mapping#*:}"
  if [[ ! -f "$destination_patch" ]] || ! cmp -s "$source_patch" "$destination_patch"; then
    mkdir -p "$(dirname "$destination_patch")"
    install -m 0644 "$source_patch" "$destination_patch"
    echo "Installed source patch: $(basename "$destination_patch")"
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
  elif [[ $(basename "$patch_file") == wpe-android-cerbero-kaios-performance.patch ]] && \
       rg -q -- '-DENABLE_DFG_JIT=ON' "$CERBERO_DIR/recipes/wpewebkit.recipe" && \
       rg -q -- '-DENABLE_WEBASSEMBLY=ON' "$CERBERO_DIR/recipes/wpewebkit.recipe"; then
    echo "Cerbero patch has the current JIT/WebAssembly configuration."
  elif [[ $(basename "$patch_file") == wpe-android-cerbero-android23-buffer.patch ]] && \
       rg -q -- 'OOS_ANDROID_LEGACY_BUFFER_LIBRARY' \
         "$CERBERO_DIR/recipes/wpewebkit.recipe" \
         "$CERBERO_DIR/recipes/wpebackend-android.recipe"; then
    echo "Cerbero patch has the Android 23 buffer compatibility configuration."
  else
    echo "Cerbero checkout does not match $patch_file." >&2
    exit 1
  fi
done

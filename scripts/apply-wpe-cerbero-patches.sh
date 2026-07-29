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
  "$ROOT_DIR/system/patches/wpe-android-cerbero-context-menus-off.patch"
  "$ROOT_DIR/system/patches/wpe-android-cerbero-libdrm-off.patch"
  "$ROOT_DIR/system/patches/wpe-android-cerbero-context-menus-link.patch"
  "$ROOT_DIR/system/patches/wpe-android-cerbero-wasm-osr-option.patch"
  "$ROOT_DIR/system/patches/wpe-android-cerbero-wpebackend-seqpacket.patch"
  "$ROOT_DIR/system/patches/wpe-android-cerbero-android23-wavpack.patch"
  "$ROOT_DIR/system/patches/wpe-android-cerbero-android23-flac.patch"
  "$ROOT_DIR/system/patches/wpe-android-cerbero-openssl3.patch"
)
SOURCE_PATCHES=(
  "$ROOT_DIR/system/patches/wpewebkit-kaios-gio-unix.patch:recipes/wpewebkit/0002-KaiOS-Android-use-GioUnix.patch"
  "$ROOT_DIR/system/patches/wpewebkit-kaios-android-armv7-jit.patch:recipes/wpewebkit/0003-KaiOS-Android-ARMv7-JIT.patch"
  "$ROOT_DIR/system/patches/wpewebkit-kaios-android23-buffer.patch:recipes/wpewebkit/0004-KaiOS-Android23-legacy-buffer.patch"
  "$ROOT_DIR/system/patches/wpewebkit-context-menus-off-build.patch:recipes/wpewebkit/0005-OOS-context-menus-off-build.patch"
  "$ROOT_DIR/system/patches/wpewebkit-libdrm-off-build.patch:recipes/wpewebkit/0006-OOS-libdrm-off-build.patch"
  "$ROOT_DIR/system/patches/wpewebkit-context-menus-off-link-build.patch:recipes/wpewebkit/0007-OOS-context-menus-off-link-build.patch"
  "$ROOT_DIR/system/patches/wpewebkit-wasm-osr-option.patch:recipes/wpewebkit/0008-OOS-honor-Wasm-OSR-option.patch"
  "$ROOT_DIR/system/patches/wpebackend-android-gralloc0.patch:recipes/wpebackend-android/0001-OOS-RGB565-and-Android23-buffer.patch"
  "$ROOT_DIR/system/patches/wpebackend-android-seqpacket-ipc.patch:recipes/wpebackend-android/0002-OOS-seqpacket-renderer-IPC.patch"
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
       rg -q -- "OOS_WPE_FEATURE_OPTIONS" "$CERBERO_DIR/recipes/wpewebkit.recipe"; then
    echo "Cerbero patch has the shared feature-profile bridge."
  elif [[ $(basename "$patch_file") == wpe-android-cerbero-kaios-performance.patch ]] && \
       rg -q -- '0003-KaiOS-Android-ARMv7-JIT.patch' \
         "$CERBERO_DIR/recipes/wpewebkit.recipe"; then
    echo "Cerbero patch has the ARMv7 JIT compiler compatibility patch."
  elif [[ $(basename "$patch_file") == wpe-android-cerbero-android23-buffer.patch ]] && \
       rg -q -- 'OOS_ANDROID_LEGACY_BUFFER_LIBRARY' \
         "$CERBERO_DIR/recipes/wpewebkit.recipe" \
         "$CERBERO_DIR/recipes/wpebackend-android.recipe"; then
    echo "Cerbero patch has the Android 23 buffer compatibility configuration."
  elif [[ $(basename "$patch_file") == wpe-android-cerbero-context-menus-off.patch ]] && \
       rg -q -- '0005-OOS-context-menus-off-build.patch' \
         "$CERBERO_DIR/recipes/wpewebkit.recipe"; then
    echo "Cerbero recipe has the context-menu compile fix."
  elif [[ $(basename "$patch_file") == wpe-android-cerbero-libdrm-off.patch ]] && \
       rg -q -- '0006-OOS-libdrm-off-build.patch' \
         "$CERBERO_DIR/recipes/wpewebkit.recipe"; then
    echo "Cerbero recipe has the libdrm-disabled compile fix."
  elif [[ $(basename "$patch_file") == wpe-android-cerbero-context-menus-link.patch ]] && \
       rg -q -- '0007-OOS-context-menus-off-link-build.patch' \
         "$CERBERO_DIR/recipes/wpewebkit.recipe"; then
    echo "Cerbero recipe has the context-menu link fix."
  elif [[ $(basename "$patch_file") == wpe-android-cerbero-wasm-osr-option.patch ]] && \
       rg -q -- '0008-OOS-honor-Wasm-OSR-option.patch' \
         "$CERBERO_DIR/recipes/wpewebkit.recipe"; then
    echo "Cerbero recipe honors the WebAssembly OSR option."
  elif [[ $(basename "$patch_file") == wpe-android-cerbero-wpebackend-seqpacket.patch ]] && \
       rg -q -- '0002-OOS-seqpacket-renderer-IPC.patch' \
         "$CERBERO_DIR/recipes/wpebackend-android.recipe"; then
    echo "WPEBackend Android recipe preserves IPC boundaries and compositor-thread replies."
  elif [[ $(basename "$patch_file") == wpe-android-cerbero-android23-wavpack.patch ]] && \
       rg -q -- 'HAVE_FSEEKO=OFF' "$CERBERO_DIR/recipes/wavpack.recipe"; then
    echo "Cerbero recipe has the Android 23 WavPack compatibility fix."
  elif [[ $(basename "$patch_file") == wpe-android-cerbero-android23-flac.patch ]] && \
       rg -q -- 'Dfseeko=fseek' "$CERBERO_DIR/recipes/flac.recipe"; then
    echo "Cerbero recipe has the Android 23 FLAC compatibility fix."
  elif [[ $(basename "$patch_file") == wpe-android-cerbero-openssl3.patch ]] && \
       rg -q -- "version = '3.5.7'" "$CERBERO_DIR/recipes/openssl.recipe"; then
    echo "Cerbero recipe uses OpenSSL 3 for WebRTC."
  else
    echo "Cerbero checkout does not match $patch_file." >&2
    exit 1
  fi
done

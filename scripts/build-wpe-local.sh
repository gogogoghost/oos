#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT_DIR/third_party/versions.env"
source "$ROOT_DIR/scripts/lib/wpe-features.sh"
oos_wpe_load_features "$ROOT_DIR/system/config/wpe/features.conf"
source "$ROOT_DIR/system/config/wpe/devices/local.env"

[[ $OOS_WPE_PROFILE == linux-x86_64-jit ]] || {
  echo "Invalid local WPE profile mapping: $OOS_WPE_PROFILE" >&2
  exit 2
}
SYSROOT=${OOS_WPE_LOCAL_SYSROOT:-"$ROOT_DIR/build/wpe-sysroot/$OOS_WPE_SYSROOT_KEY"}
PREFIX=/opt/oos
INSTALL_PREFIX="$SYSROOT$PREFIX"
BUILD_ROOT=${OOS_WPE_LOCAL_BUILD_ROOT:-"$ROOT_DIR/build/wpe-build/local"}
HOST_THREADS=$(nproc)
DEFAULT_JOBS=$((HOST_THREADS < OOS_WPE_DEFAULT_JOBS ? HOST_THREADS : OOS_WPE_DEFAULT_JOBS))
JOBS=${WPE_BUILD_JOBS:-$DEFAULT_JOBS}
FEATURE_SHA=$(sha256sum "$ROOT_DIR/system/config/wpe/features.conf" | awk '{print $1}')
SOURCE_PATCHES=(
  "$ROOT_DIR/system/patches/wpewebkit-context-menus-off-build.patch"
  "$ROOT_DIR/system/patches/wpewebkit-libdrm-off-build.patch"
  "$ROOT_DIR/system/patches/wpewebkit-context-menus-off-link-build.patch"
  "$ROOT_DIR/system/patches/wpewebkit-oos-eager-wasm-bbq.patch"
)
SOURCE_PATCH_SHA=$(sha256sum "${SOURCE_PATCHES[@]}" | awk '{print $1}' |
  sha256sum | awk '{print $1}')
READY="$INSTALL_PREFIX/.oos-wpe-profile"

if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "WPE_BUILD_JOBS must be a positive integer: $JOBS" >&2
  exit 2
fi

profile_matches() {
  [[ -f "$READY" && -f "$INSTALL_PREFIX/lib/libWPEWebKit-2.0.so" &&
     -x "$INSTALL_PREFIX/libexec/wpe-webkit-2.0/WPEWebProcess" ]] || return 1
  grep -qx "profile=$OOS_WPE_PROFILE" "$READY" &&
    grep -qx "buffer_abi=$OOS_WPE_BUFFER_ABI" "$READY" &&
    grep -qx "display_backend=$OOS_WPE_DISPLAY_BACKEND" "$READY" &&
    grep -qx "wpewebkit=$WPEWEBKIT_VERSION" "$READY" &&
    grep -qx "libwpe=$LIBWPE_VERSION" "$READY" &&
    grep -qx "wpebackend_fdo=$WPEBACKEND_FDO_VERSION" "$READY" &&
    grep -qx "features_sha256=$FEATURE_SHA" "$READY" &&
    grep -qx "source_patch_sha256=$SOURCE_PATCH_SHA" "$READY"
}

source_patch_is_ready() {
  local patch_name=$1
  local media_controls="$ROOT_DIR/third_party/wpewebkit/Source/WebCore/Modules/mediacontrols/MediaControlsHost.cpp"
  local backing_store="$ROOT_DIR/third_party/wpewebkit/Source/WebKit/UIProcess/wpe/AcceleratedBackingStore.cpp"

  case "$patch_name" in
    wpewebkit-context-menus-off-build.patch)
      # The follow-up link fix extends this same preprocessor branch, so its
      # final marker also proves that the prerequisite return fix is present.
      grep -Fq 'UNUSED_PARAM(target);' "$media_controls"
      ;;
    wpewebkit-context-menus-off-link-build.patch)
      grep -Fq 'UNUSED_PARAM(optionsJSONString);' "$media_controls" &&
        grep -Fq 'UNUSED_PARAM(callback);' "$media_controls"
      ;;
    wpewebkit-libdrm-off-build.patch)
      grep -Fq 'static constexpr uint32_t xRGB8888Format' "$backing_store" &&
        grep -Fq '== xRGB8888Format' "$backing_store"
      ;;
    wpewebkit-oos-eager-wasm-bbq.patch)
      grep -Fq 'useEagerBBQCompilation' \
        "$ROOT_DIR/third_party/wpewebkit/Source/JavaScriptCore/runtime/OptionsList.h" &&
        grep -Fq 'eagerlyCompileBBQ' \
        "$ROOT_DIR/third_party/wpewebkit/Source/JavaScriptCore/wasm/WasmModule.cpp"
      ;;
    *)
      return 1
      ;;
  esac
}

if profile_matches && [[ ${OOS_WPE_FORCE_REBUILD:-0} != 1 ]]; then
  echo "Pinned local WPE sysroot is ready at $SYSROOT"
  exit 0
fi

required_commands=(
  bison cmake curl flex gperf meson ninja patch perl pkg-config python3 ruby
  sha256sum tar unifdef)
missing_commands=()
for command in "${required_commands[@]}"; do
  command -v "$command" >/dev/null 2>&1 || missing_commands+=("$command")
done
if [[ ${#missing_commands[@]} -ne 0 ]]; then
  echo "Missing local WPE build commands:" >&2
  printf '  %s\n' "${missing_commands[@]}" >&2
  exit 1
fi
if ! perl -MJSON::PP -e 1 2>/dev/null; then
  echo "Missing Perl module JSON::PP (Fedora package: perl-JSON-PP)." >&2
  exit 1
fi
if ! perl -Mbigint -e 1 2>/dev/null; then
  echo "Missing Perl module bigint (Fedora package: perl-bignum)." >&2
  exit 1
fi

required_packages=(
  egl epoxy fontconfig freetype2 gio-2.0 glib-2.0 gnutls
  gstreamer-1.0 gstreamer-app-1.0 gstreamer-audio-1.0
  gstreamer-gl-1.0 gstreamer-pbutils-1.0 gstreamer-video-1.0
  gstreamer-webrtc-1.0 harfbuzz icu-i18n libgcrypt libjpeg libpng
  libpsl libsoup-3.0 libsecret-1 libtasn1 libwebp libxml-2.0 libxslt
  sqlite3 wayland-client wayland-egl xkbcommon zlib)
missing_packages=()
for package in "${required_packages[@]}"; do
  pkg-config --exists "$package" || missing_packages+=("$package")
done
if [[ ${#missing_packages[@]} -ne 0 ]]; then
  echo "Missing local WPE development packages (pkg-config names):" >&2
  printf '  %s\n' "${missing_packages[@]}" >&2
  exit 1
fi

"$ROOT_DIR/scripts/fetch-wpe.sh" local
mkdir -p "$INSTALL_PREFIX" "$BUILD_ROOT"

for source_patch in "${SOURCE_PATCHES[@]}"; do
  patch_name=$(basename "$source_patch")
  if source_patch_is_ready "$patch_name" ||
    patch --batch --silent --forward --reverse --dry-run -d \
      "$ROOT_DIR/third_party/wpewebkit" -p1 <"$source_patch" \
      >/dev/null 2>&1; then
    echo "WPEWebKit source patch is ready: $patch_name"
  elif patch --batch --silent --dry-run -d \
      "$ROOT_DIR/third_party/wpewebkit" -p1 <"$source_patch" \
      >/dev/null 2>&1; then
    patch --batch --silent -d "$ROOT_DIR/third_party/wpewebkit" -p1 \
      <"$source_patch"
    echo "Applied WPEWebKit source patch: $patch_name"
  else
    echo "Pinned WPEWebKit source does not match $source_patch" >&2
    exit 1
  fi
done

HOST_PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-}
export CCACHE_DISABLE=${CCACHE_DISABLE:-1}

libwpe_setup=()
[[ -f "$BUILD_ROOT/libwpe/meson-private/coredata.dat" ]] &&
  libwpe_setup+=(--reconfigure)
meson setup "$BUILD_ROOT/libwpe" "$ROOT_DIR/third_party/libwpe" \
  --prefix "$PREFIX" --libdir lib --buildtype release \
  -Dbuild-docs=false -Denable-xkb=true \
  -Ddefault-backend=libWPEBackend-fdo-1.0.so "${libwpe_setup[@]}"
meson compile -C "$BUILD_ROOT/libwpe" -j "$JOBS"
DESTDIR="$SYSROOT" meson install -C "$BUILD_ROOT/libwpe"

export PKG_CONFIG_PATH="$BUILD_ROOT/libwpe/meson-uninstalled${HOST_PKG_CONFIG_PATH:+:$HOST_PKG_CONFIG_PATH}"
fdo_setup=()
[[ -f "$BUILD_ROOT/wpebackend-fdo/meson-private/coredata.dat" ]] &&
  fdo_setup+=(--reconfigure)
meson setup "$BUILD_ROOT/wpebackend-fdo" \
  "$ROOT_DIR/third_party/wpebackend-fdo" --prefix "$PREFIX" --libdir lib \
  --buildtype release -Dbuild_docs=false "${fdo_setup[@]}"
meson compile -C "$BUILD_ROOT/wpebackend-fdo" -j "$JOBS"
DESTDIR="$SYSROOT" meson install -C "$BUILD_ROOT/wpebackend-fdo"

cmake -S "$ROOT_DIR/third_party/wpewebkit" \
  -B "$BUILD_ROOT/wpewebkit" -G Ninja \
  -DPORT=WPE \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DWPE_INCLUDE_DIR="$ROOT_DIR/third_party/libwpe/include" \
  -DWPE_LIBRARY="$BUILD_ROOT/libwpe/libwpe-1.0.so" \
  "${OOS_WPE_CMAKE_OPTIONS[@]}"
cmake --build "$BUILD_ROOT/wpewebkit" -j "$JOBS"
DESTDIR="$SYSROOT" cmake --install "$BUILD_ROOT/wpewebkit"

oos_wpe_verify_features "$BUILD_ROOT/wpewebkit/cmakeconfig.h"
printf '%s\n' \
  "profile=$OOS_WPE_PROFILE" \
  "buffer_abi=$OOS_WPE_BUFFER_ABI" \
  "display_backend=$OOS_WPE_DISPLAY_BACKEND" \
  "wpewebkit=$WPEWEBKIT_VERSION" \
  "libwpe=$LIBWPE_VERSION" \
  "wpebackend_fdo=$WPEBACKEND_FDO_VERSION" \
  "features_sha256=$FEATURE_SHA" \
  "source_patch_sha256=$SOURCE_PATCH_SHA" \
  >"$READY"
echo "Pinned local WPE sysroot is ready at $SYSROOT"

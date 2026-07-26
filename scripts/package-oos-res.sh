#!/usr/bin/env bash

set -euo pipefail
umask 022

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT_DIR/scripts/lib/package-common.sh"

DEVICE=nokia-2780-flip
OUTPUT_DIR=
VERSION=
CREATE_TGZ=0
ACTIVATE=0

usage() {
  echo "usage: $0 VERSION [--device DEVICE] [--output OOS_DIR] [--tgz] [--activate]" >&2
}

if [[ $# -gt 0 && "$1" != -* ]]; then
  VERSION=$1
  shift
fi
while [[ $# -gt 0 ]]; do
  case "$1" in
    --device)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      DEVICE=$2
      shift 2
      ;;
    --output)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      OUTPUT_DIR=$2
      shift 2
      ;;
    --tgz)
      CREATE_TGZ=1
      shift
      ;;
    --activate)
      ACTIVATE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage
      exit 2
      ;;
  esac
done

[[ "$DEVICE" == nokia-2780-flip ]] ||
  package_die "Res packaging is not implemented for $DEVICE"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-.+][0-9A-Za-z.-]+)?$ ]] ||
  package_die "Invalid or missing res version: $VERSION"

OUTPUT_DIR=${OUTPUT_DIR:-$ROOT_DIR/dist/$DEVICE/oos}
package_require_directory "$OUTPUT_DIR"

RES_NAME="res-$VERSION"
DESTINATION="$OUTPUT_DIR/$RES_NAME"
[[ ! -e "$DESTINATION" ]] || package_die "Res output already exists: $DESTINATION"

OOS_BINARY="$ROOT_DIR/build/android-$DEVICE/bin/oos"
WPE_SYSROOT="$ROOT_DIR/build/wpe-sysroot/$DEVICE"
NATIVE_APPS="$ROOT_DIR/build/native-apps"
BOOT_SPLASH="$ROOT_DIR/assets/boot/$DEVICE/boot-splash.png"
LUCIDE_LICENSE="$ROOT_DIR/LICENSES/Lucide.txt"
WAMR_LICENSE="$ROOT_DIR/third_party/wasm-micro-runtime/LICENSE"
package_require_file "$OOS_BINARY"
package_require_directory "$WPE_SYSROOT/lib"
package_require_directory "$WPE_SYSROOT/libexec"
"$ROOT_DIR/scripts/build-native-app-aot.sh"
package_require_file "$NATIVE_APPS/launcher.wasm"
package_require_file "$NATIVE_APPS/launcher.aot"
package_require_file "$BOOT_SPLASH"
package_require_file "$LUCIDE_LICENSE"
package_require_file "$WAMR_LICENSE"

if [[ -f "$ROOT_DIR/.env" ]]; then
  set -a
  source "$ROOT_DIR/.env"
  set +a
fi
WPE_NDK=${WPE_NDK:-/home/jax/Android/Sdk/ndk/magisk}
CXX_RUNTIME="$WPE_NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/arm-linux-androideabi/libc++_shared.so"
if [[ ! -f "$CXX_RUNTIME" ]]; then
  CXX_RUNTIME="$WPE_NDK/sources/cxx-stl/llvm-libc++/libs/armeabi-v7a/libc++_shared.so"
fi
package_require_file "$CXX_RUNTIME"
SYSTEM_DIR=${SYSTEM_DIR:-}
package_require_directory "$SYSTEM_DIR/lib"

STAGING_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/oos-res.XXXXXX")
cleanup() {
  rm -rf "$STAGING_ROOT"
  if [[ -n "${ACTIVATE_TMP:-}" ]]; then
    rm -f "$ACTIVATE_TMP"
  fi
}
trap cleanup EXIT
STAGING="$STAGING_ROOT/$RES_NAME"
mkdir -p "$STAGING/bin" "$STAGING/lib" "$STAGING/libexec" \
  "$STAGING/apps" "$STAGING/share/oos" "$STAGING/share/licenses/oos" \
  "$STAGING/etc"

install -m 0644 "$NATIVE_APPS/launcher.wasm" "$STAGING/apps/launcher.wasm"
install -m 0644 "$NATIVE_APPS/launcher.aot" "$STAGING/apps/launcher.aot"
install -m 0644 "$BOOT_SPLASH" "$STAGING/share/oos/boot-splash.png"
install -m 0644 "$LUCIDE_LICENSE" \
  "$STAGING/share/licenses/oos/Lucide.txt"
install -m 0644 "$WAMR_LICENSE" \
  "$STAGING/share/licenses/oos/WAMR.txt"

declare -A COPIED_ELF=()
declare -a ELF_QUEUE=()

copy_runtime_elf() {
  local source_file=$1
  local relative_path=$2
  local destination_file="$STAGING/$relative_path"
  [[ -f "$source_file" ]] || package_die "Missing runtime ELF: $source_file"
  if [[ -n ${COPIED_ELF[$relative_path]+x} ]]; then
    return
  fi
  mkdir -p "$(dirname "$destination_file")"
  cp -L --preserve=mode,timestamps "$source_file" "$destination_file"
  COPIED_ELF[$relative_path]=1
  ELF_QUEUE+=("$destination_file")
}

find_sysroot_library() {
  local library_name=$1
  find "$WPE_SYSROOT/lib" -maxdepth 1 \( -type f -o -type l \) \
    -name "$library_name" -print -quit
}

system_provides_library() {
  local library_name=$1
  find "$SYSTEM_DIR/lib" -maxdepth 1 \( -type f -o -type l \) \
    -name "$library_name" -print -quit | grep -q .
}

copy_runtime_elf "$OOS_BINARY" bin/oos
copy_runtime_elf "$CXX_RUNTIME" lib/libc++_shared.so
for runtime_entry in \
  lib/libWPEBackend-android.so \
  lib/wpe-webkit-2.0/injected-bundle/libWPEInjectedBundle.so \
  lib/gio/modules/libgioenvironmentproxy.so \
  lib/gio/modules/libgioopenssl.so \
  libexec/wpe-webkit-2.0/WPENetworkProcess \
  libexec/wpe-webkit-2.0/WPEWebProcess; do
  copy_runtime_elf "$WPE_SYSROOT/$runtime_entry" "$runtime_entry"
done

queue_index=0
while (( queue_index < ${#ELF_QUEUE[@]} )); do
  runtime_file=${ELF_QUEUE[$queue_index]}
  ((queue_index += 1))
  while IFS= read -r needed_library; do
    [[ -n "$needed_library" ]] || continue
    if [[ -n ${COPIED_ELF[lib/$needed_library]+x} ]]; then
      continue
    fi
    if [[ "$needed_library" == libc++_shared.so ]]; then
      copy_runtime_elf "$CXX_RUNTIME" "lib/$needed_library"
      continue
    fi
    sysroot_library=$(find_sysroot_library "$needed_library")
    if [[ -n "$sysroot_library" ]]; then
      copy_runtime_elf "$sysroot_library" "lib/$needed_library"
    elif ! system_provides_library "$needed_library"; then
      package_die "Cannot resolve $needed_library required by $runtime_file"
    fi
  done < <(readelf -d "$runtime_file" 2>/dev/null \
    | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
done

for runtime_share in fontconfig glib-2.0 icu licenses xml; do
  if [[ -d "$WPE_SYSROOT/share/$runtime_share" ]]; then
    rsync -a "$WPE_SYSROOT/share/$runtime_share" "$STAGING/share/"
  fi
done
if [[ -d "$WPE_SYSROOT/etc" ]]; then
  rsync -a "$WPE_SYSROOT/etc/" "$STAGING/etc/"
fi

package_verify_elf_dependencies "$STAGING" "$SYSTEM_DIR"

if [[ -n ${WPE_STRIP:-} ]]; then
  STRIP_TOOL=$WPE_STRIP
elif [[ -x "$WPE_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" ]]; then
  STRIP_TOOL="$WPE_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
elif command -v llvm-strip >/dev/null 2>&1; then
  STRIP_TOOL=$(command -v llvm-strip)
else
  package_die "Cannot find llvm-strip; set WPE_STRIP in .env"
fi
for runtime_file in "${ELF_QUEUE[@]}"; do
  "$STRIP_TOOL" --strip-unneeded "$runtime_file"
done
echo "Packaged and stripped ${#ELF_QUEUE[@]} runtime ELF files"

printf '%s\n' \
  "format=1" \
  "type=oos-res" \
  "version=$VERSION" \
  "device=$DEVICE" \
  "abi=armeabi-v7a" \
  "android_api=29" \
  "javascript_jit=baseline,dfg" \
  "webassembly_jit=bbq" \
  "native_app_runtime=wamr-2.4.4" \
  "native_app_execution=aot" \
  "launcher_framework=egui-0.35" \
  "runtime_prefix=/opt/oos" \
  "git_commit=$(git -C "$ROOT_DIR" rev-parse HEAD)" \
  > "$STAGING/manifest.env"
package_write_checksums "$STAGING"

mv "$STAGING" "$DESTINATION"

if [[ "$ACTIVATE" -eq 1 ]]; then
  ACTIVATE_TMP="$OUTPUT_DIR/.res.$$.new"
  ln -s "$RES_NAME" "$ACTIVATE_TMP"
  mv -Tf "$ACTIVATE_TMP" "$OUTPUT_DIR/res"
  ACTIVATE_TMP=
  echo "Activated $RES_NAME"
fi

if [[ "$CREATE_TGZ" -eq 1 ]]; then
  ARCHIVE="$(dirname "$OUTPUT_DIR")/oos-res-$DEVICE-$VERSION.tgz"
  package_create_tgz "$DESTINATION" "$ARCHIVE"
  echo "Created $ARCHIVE"
fi
echo "Created $DESTINATION"

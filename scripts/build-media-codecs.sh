#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TARGET=${1:-local}
source "$ROOT_DIR/third_party/versions.env"
[[ -x "$ROOT_DIR/third_party/ffmpeg/configure" ]] ||
  "$ROOT_DIR/scripts/fetch-media-dependencies.sh"

BUILD_DIR="$ROOT_DIR/build/media-codecs/$TARGET/build"
PREFIX="$ROOT_DIR/build/media-codecs/$TARGET/install"
STAMP="$PREFIX/.oos-ffmpeg-revision"
EXPECTED_REVISION="$FFMPEG_COMMIT-oos3-shared"
if [[ -f "$STAMP" && $(<"$STAMP") == "$EXPECTED_REVISION" &&
      -f "$PREFIX/lib/libavformat.so" ]]; then
  echo "FFmpeg media codecs are ready for $TARGET"
  exit 0
fi

mkdir -p "$BUILD_DIR" "$PREFIX"
COMMON=(
  --prefix="$PREFIX"
  --disable-everything
  --disable-programs
  --disable-doc
  --disable-debug
  --disable-network
  --disable-autodetect
  --enable-pthreads
  --enable-small
  --disable-static
  --enable-shared
  --enable-pic
  --enable-avcodec
  --enable-avformat
  --enable-avutil
  --enable-swresample
  --disable-avdevice
  --disable-avfilter
  --disable-swscale
  --enable-protocol=file
  --enable-decoder=aac,aac_fixed,aac_latm,amrnb,amrwb,vorbis,opus
  --enable-demuxer=aac,amr,amrnb,amrwb,mov,ogg
  --enable-parser=aac,aac_latm,opus,vorbis
)

TARGET_ARGS=()
case "$TARGET" in
  local)
    ;;
  nokia-2780-flip|nokia-8110-4g)
    source "$ROOT_DIR/.env"
    ANDROID_NDK=${ANDROID_NDK:-/home/jax/Android/Sdk/ndk/r21e}
    API=29
    [[ $TARGET == nokia-8110-4g ]] && API=23
    TOOLCHAIN="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
    TARGET_ARGS=(
      --target-os=android
      --arch=arm
      --cpu=armv7-a
      --enable-cross-compile
      --cc="$TOOLCHAIN/armv7a-linux-androideabi${API}-clang"
      --ar="$TOOLCHAIN/llvm-ar"
      --ranlib="$TOOLCHAIN/llvm-ranlib"
      --strip="$TOOLCHAIN/llvm-strip")
    ;;
  *)
    echo "unknown media-codec target: $TARGET" >&2
    exit 2
    ;;
esac

(
  cd "$BUILD_DIR"
  "$ROOT_DIR/third_party/ffmpeg/configure" "${COMMON[@]}" "${TARGET_ARGS[@]}"
  make -j"${OOS_BUILD_JOBS:-24}"
  make install
)
printf '%s\n' "$EXPECTED_REVISION" > "$STAMP"
echo "Built FFmpeg media codecs for $TARGET in $PREFIX"

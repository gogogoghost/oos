#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ADB=${ADB:-adb}
DEVICE=nokia-2780-flip
ACTION=start

if [[ ${1:-} == nokia-* ]]; then
  DEVICE=$1
  shift
fi
if [[ $# -gt 0 ]]; then
  ACTION=$1
  shift
fi
[[ $# -eq 0 ]] || { echo "usage: $0 [DEVICE] {start|input-test|stop|status}" >&2; exit 2; }

case "$DEVICE" in
  nokia-2780-flip|nokia-8110-4g) ;;
  *) echo "unsupported WPE device: $DEVICE" >&2; exit 2 ;;
esac
case "$ACTION" in
  start|input-test|stop|status) ;;
  *) echo "usage: $0 [DEVICE] {start|input-test|stop|status}" >&2; exit 2 ;;
esac

REMOTE=/data/local/tmp/oos-wpe
DEVICE_SCRIPT="$ROOT_DIR/scripts/device/wpe_chroot_device.sh"
WPE_SYSROOT="$ROOT_DIR/build/wpe-sysroot/$DEVICE"
WPE_PRODUCER="$ROOT_DIR/build/android-$DEVICE/bin/tests/$DEVICE/oos_test_${DEVICE//-/_}_wpe_producer"
WPE_HOST="$ROOT_DIR/build/android-$DEVICE/bin/tests/$DEVICE/oos_test_${DEVICE//-/_}_wpe_host"
INPUT_TEST="$ROOT_DIR/build/android-$DEVICE/bin/tests/$DEVICE/oos_test_nokia_2780_key_input"
WPE_HTML="$ROOT_DIR/tests/web/assets/hello.html"

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

adb_root_shell() {
  local command=$1
  local uid
  uid=$($ADB shell id -u 2>/dev/null | tr -d '\r')
  if [[ $uid == 0 ]]; then
    "$ADB" shell "$command"
  else
    "$ADB" shell "su -c '$command'"
  fi
}

if [[ $ACTION == stop || $ACTION == status ]]; then
  adb_root_shell "if [ -x $REMOTE/wpe_chroot_device.sh ]; then $REMOTE/wpe_chroot_device.sh $ACTION; fi"
  exit 0
fi

for required in "$DEVICE_SCRIPT" "$WPE_PRODUCER" "$WPE_HOST" "$WPE_HTML" \
  "$CXX_RUNTIME" \
  "$WPE_SYSROOT/lib/libWPEBackend-android.so" \
  "$WPE_SYSROOT/lib/libWPEWebKit-2.0.so" \
  "$WPE_SYSROOT/libexec/wpe-webkit-2.0/WPEWebProcess"; do
  [[ -f "$required" ]] || { echo "missing WPE runtime file: $required" >&2; exit 1; }
done
if [[ $ACTION == input-test && ! -x $INPUT_TEST ]]; then
  echo "input test is available only after building the Nokia 2780 target" >&2
  exit 1
fi

STAGING=$(mktemp -d "${TMPDIR:-/tmp}/oos-wpe-$DEVICE.XXXXXX")
ARCHIVE=$(mktemp "${TMPDIR:-/tmp}/oos-wpe-$DEVICE.XXXXXX.tgz")
cleanup() {
  rm -rf "$STAGING"
  rm -f "$ARCHIVE"
}
trap cleanup EXIT
mkdir -p "$STAGING/lib" "$STAGING/libexec" "$STAGING/share" "$STAGING/etc"
cp -a "$WPE_SYSROOT/lib/." "$STAGING/lib/"
cp -a "$WPE_SYSROOT/libexec/." "$STAGING/libexec/"
if [[ -d "$WPE_SYSROOT/share" ]]; then
  cp -a "$WPE_SYSROOT/share/." "$STAGING/share/"
fi
if [[ -d "$WPE_SYSROOT/etc" ]]; then
  cp -a "$WPE_SYSROOT/etc/." "$STAGING/etc/"
fi
install -m 0755 "$CXX_RUNTIME" "$STAGING/lib/libc++_shared.so"
install -m 0755 "$WPE_PRODUCER" "$STAGING/$(basename "$WPE_PRODUCER")"
install -m 0755 "$WPE_HOST" "$STAGING/$(basename "$WPE_HOST")"
if [[ -x $INPUT_TEST ]]; then
  install -m 0755 "$INPUT_TEST" "$STAGING/$(basename "$INPUT_TEST")"
fi
install -m 0644 "$WPE_HTML" "$STAGING/hello.html"
install -m 0755 "$DEVICE_SCRIPT" "$STAGING/wpe_chroot_device.sh"

source "$ROOT_DIR/config/wpe/devices/$DEVICE.env"
printf '%s\n' \
  "DEVICE='$DEVICE'" \
  "BUILD_PREFIX='/home/jax/project/oos/build/wpe-sysroot/$OOS_WPE_SYSROOT_KEY'" \
  "HELLO_PRODUCER='$(basename "$WPE_PRODUCER")'" \
  "HELLO_HOST='$(basename "$WPE_HOST")'" \
  "INPUT_PROGRAM='$(basename "$INPUT_TEST")'" \
  > "$STAGING/device.env"
tar -C "$STAGING" -czf "$ARCHIVE" .

adb_root_shell "setprop ctl.stop b2g; setprop ctl.stop b2gkillerd; mkdir -p $REMOTE"
"$ADB" push "$ARCHIVE" "$REMOTE/runtime.tgz" >/dev/null
adb_root_shell "set -e; if [ -x $REMOTE/wpe_chroot_device.sh ]; then $REMOTE/wpe_chroot_device.sh stop 2>/dev/null || true; fi; if command -v busybox >/dev/null 2>&1; then busybox tar -xzf $REMOTE/runtime.tgz -C $REMOTE; elif command -v gunzip >/dev/null 2>&1; then tar -xzf $REMOTE/runtime.tgz -C $REMOTE; elif command -v gzip >/dev/null 2>&1; then gzip -d -c $REMOTE/runtime.tgz | tar -xf - -C $REMOTE; else echo 'No gzip decompressor is available' >&2; exit 1; fi; test -f $REMOTE/wpe_chroot_device.sh; chmod 0755 $REMOTE/wpe_chroot_device.sh; $REMOTE/wpe_chroot_device.sh $ACTION"
sleep 2
adb_root_shell "$REMOTE/wpe_chroot_device.sh status"

#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DEVICE_SCRIPT="$ROOT_DIR/devices/nokia-2780-flip/scripts/wpe_chroot_device.sh"
WPE_POC="$ROOT_DIR/build/android-nokia-2780-flip/bin/nokia-2780-flip/nokia_2780_wpe_hello"
WPE_INJECTED_BUNDLE="$ROOT_DIR/build/wpe-sysroot/nokia-2780-flip/lib/wpe-webkit-2.0/injected-bundle/libWPEInjectedBundle.so"
WPE_BACKEND="$ROOT_DIR/build/wpe-sysroot/nokia-2780-flip/lib/libWPEBackend-android.so"
WPE_HTML="$ROOT_DIR/devices/nokia-2780-flip/assets/hello.html"
REMOTE=/data/local/tmp/oos-wpe
ADB=${ADB:-adb}
# fb1 is a direct framebuffer path and conflicts with the active primary SPI
# panel. Keep the diagnostic cover path opt-in until the vendor display stack
# exposes it as a real HWC display.
OOS_ENABLE_COVER=${OOS_ENABLE_COVER:-0}

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
if [[ ! -x "$WPE_POC" ]]; then
  echo "Missing WPE POC binary: $WPE_POC" >&2
  exit 1
fi
if [[ ! -f "$WPE_INJECTED_BUNDLE" ]]; then
  echo "Missing WPE injected bundle: $WPE_INJECTED_BUNDLE" >&2
  exit 1
fi
if [[ ! -f "$WPE_BACKEND" ]]; then
  echo "Missing WPE Android backend: $WPE_BACKEND" >&2
  exit 1
fi
if [[ ! -f "$WPE_HTML" ]]; then
  echo "Missing WPE HTML fixture: $WPE_HTML" >&2
  exit 1
fi
if [[ ! -f "$CXX_RUNTIME" ]]; then
  echo "Cannot find ARMv7 libc++_shared.so in WPE_NDK=$WPE_NDK" >&2
  exit 1
fi

case "${1:-start}" in
  start)
    "$ADB" shell "su -c 'mkdir -p $REMOTE/lib/wpe-webkit-2.0/injected-bundle; chmod 0777 $REMOTE $REMOTE/lib $REMOTE/lib/wpe-webkit-2.0 $REMOTE/lib/wpe-webkit-2.0/injected-bundle'"
    "$ADB" push "$CXX_RUNTIME" "$REMOTE/lib/libc++_shared.so" >/dev/null
    "$ADB" push "$WPE_POC" "$REMOTE/nokia_2780_wpe_hello" >/dev/null
    "$ADB" push "$WPE_BACKEND" "$REMOTE/lib/libWPEBackend-android.so" >/dev/null
    "$ADB" push "$WPE_INJECTED_BUNDLE" "$REMOTE/lib/wpe-webkit-2.0/injected-bundle/libWPEInjectedBundle.so" >/dev/null
    "$ADB" push "$WPE_HTML" "$REMOTE/hello.html" >/dev/null
    "$ADB" push "$DEVICE_SCRIPT" "$REMOTE/wpe_chroot_device.sh" >/dev/null
    "$ADB" shell "su -c 'chmod 0755 $REMOTE/wpe_chroot_device.sh; OOS_ENABLE_COVER=$OOS_ENABLE_COVER $REMOTE/wpe_chroot_device.sh start'"
    sleep 2
    "$ADB" shell "su -c '$REMOTE/wpe_chroot_device.sh status'"
    ;;
  stop|status)
    "$ADB" shell "su -c '$REMOTE/wpe_chroot_device.sh $1'"
    ;;
  *)
    echo "usage: $0 {start|stop|status}" >&2
    exit 2
    ;;
esac

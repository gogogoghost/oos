#!/system/bin/sh

set -eu

OOS_HOME=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$OOS_HOME/bootstrap.sh"

acquire_bootstrap_lock start
trap 'release_bootstrap_lock' 0

if [ -f "$OOS_PID_FILE" ]; then
  existing_pid=$(cat "$OOS_PID_FILE" 2>/dev/null || true)
  if oos_pid_running "$existing_pid"; then
    echo "OOS is already running with pid $existing_pid" >&2
    exit 1
  fi
  rm -f "$OOS_PID_FILE"
fi

# init.sh runs as a child of this shell and therefore shares the mount
# namespace used by the chroot below.
"$OOS_HOME/init.sh"
resolve_res

runtime_config_value() {
  awk -F= -v option="$1" \
    '$1 == option { value=substr($0, index($0, "=") + 1) } END { print value }' \
    "$OOS_PERSIST_DIR/system/runtime.conf"
}

if [ -f "$OOS_PERSIST_DIR/system/runtime.conf" ]; then
  if [ -z "${OOS_ENABLE_INSPECTOR+x}" ]; then
    OOS_ENABLE_INSPECTOR=$(runtime_config_value OOS_ENABLE_INSPECTOR)
  fi
  if [ -z "${OOS_TRACE_WEB_CONSOLE+x}" ]; then
    OOS_TRACE_WEB_CONSOLE=$(runtime_config_value OOS_TRACE_WEB_CONSOLE)
  fi
  if [ -z "${OOS_TRACE_DEVICE_API+x}" ]; then
    OOS_TRACE_DEVICE_API=$(runtime_config_value OOS_TRACE_DEVICE_API)
  fi
  if [ -z "${OOS_TRACE_WPE_FRAMES+x}" ]; then
    OOS_TRACE_WPE_FRAMES=$(runtime_config_value OOS_TRACE_WPE_FRAMES)
  fi
  if [ -z "${OOS_TRACE_WEBAUDIO+x}" ]; then
    OOS_TRACE_WEBAUDIO=$(runtime_config_value OOS_TRACE_WEBAUDIO)
  fi
  if [ -z "${OOS_INSPECTOR_ADDRESS+x}" ]; then
    OOS_INSPECTOR_ADDRESS=$(runtime_config_value OOS_INSPECTOR_ADDRESS)
  fi
  if [ -z "${OOS_WEB_AUDIO_NICE+x}" ]; then
    OOS_WEB_AUDIO_NICE=$(runtime_config_value OOS_WEB_AUDIO_NICE)
  fi
fi
for runtime_switch in "${OOS_ENABLE_INSPECTOR:-0}" \
  "${OOS_TRACE_WEB_CONSOLE:-0}" "${OOS_TRACE_DEVICE_API:-0}" \
  "${OOS_TRACE_WPE_FRAMES:-0}" "${OOS_TRACE_WEBAUDIO:-0}"; do
  case "$runtime_switch" in
    0|1) ;;
    *) echo "invalid boolean in runtime.conf: $runtime_switch" >&2; exit 1 ;;
  esac
done
case "${OOS_INSPECTOR_ADDRESS:-127.0.0.1:9222}" in
  127.0.0.1:*) ;;
  *) echo "inspector must listen on device loopback" >&2; exit 1 ;;
esac
OOS_WEB_AUDIO_NICE=${OOS_WEB_AUDIO_NICE:--10}
case "$OOS_WEB_AUDIO_NICE" in
  -[0-9]|-[0-9][0-9]|[0-9]|[0-9][0-9]) ;;
  *) echo "invalid OOS_WEB_AUDIO_NICE: $OOS_WEB_AUDIO_NICE" >&2; exit 1 ;;
esac
if [ "$OOS_WEB_AUDIO_NICE" -lt -20 ] || [ "$OOS_WEB_AUDIO_NICE" -gt 19 ]; then
  echo "OOS_WEB_AUDIO_NICE must be between -20 and 19" >&2
  exit 1
fi
export OOS_ENABLE_INSPECTOR OOS_TRACE_WEB_CONSOLE OOS_TRACE_DEVICE_API
export OOS_TRACE_WPE_FRAMES
export OOS_TRACE_WEBAUDIO
export OOS_INSPECTOR_ADDRESS
export OOS_WEB_AUDIO_NICE

setprop ctl.stop b2g 2>/dev/null || true
setprop ctl.stop b2gkillerd 2>/dev/null || true
echo 0 > /sys/class/leds/lcd-backlight/brightness 2>/dev/null || true
if [ -e /sys/class/leds/sublcd-backlight/brightness ]; then
  echo 0 > /sys/class/leds/sublcd-backlight/brightness
fi
if [ -e /sys/class/graphics/fb1/blank ]; then
  echo 4 > /sys/class/graphics/fb1/blank
fi

if [ -n "${OOS_HWC_SERVICE:-}" ]; then
  setprop ctl.restart "$OOS_HWC_SERVICE"
  wait_for_service "$OOS_HWC_SERVICE" 50
fi

release_bootstrap_lock
trap - 0

export OOS_RES_VERSION=$(sed -n 's/^version=//p' "$OOS_RES_DIR/manifest.env")
export HOME=/data/users/0/system
export TMPDIR=/data/tmp
export XDG_CACHE_HOME=/data/cache/system
export XDG_DATA_HOME=/data/users/0/system/share
# The host owns the Android display HAL. System libraries must win here so a
# WPE dependency (notably libpng) cannot interpose on a HAL dependency. The
# WPE child switches back to its private runtime-first path before exec.
export LD_LIBRARY_PATH=/system/lib:/vendor/lib:/apex/com.android.runtime/lib:/opt/oos/lib
export OOS_WPE_LD_LIBRARY_PATH=/opt/oos/lib:/system/lib:/vendor/lib:/apex/com.android.runtime/lib
export WEBKIT_EXEC_PATH=/opt/oos/libexec/wpe-webkit-2.0
export WEBKIT_INJECTED_BUNDLE_PATH=/opt/oos/lib/wpe-webkit-2.0/injected-bundle
export WPE_BACKEND=/opt/oos/lib/libWPEBackend-android.so
export GIO_EXTRA_MODULES=/opt/oos/lib/gio/modules
export GST_PLUGIN_SYSTEM_PATH=/opt/oos/lib/gstreamer-1.0
export GST_PLUGIN_SCANNER=/opt/oos/libexec/gstreamer-1.0/gst-plugin-scanner
export GST_REGISTRY=/data/cache/system/gstreamer-1.0/registry.bin
export OOS_WAMR_AOT_CACHE_PATH=/data/cache/webassembly-aot:/opt/oos/share/oos/webassembly-aot
export FONTCONFIG_FILE=/opt/oos/etc/fonts/fonts.conf
export SSL_CERT_FILE=/opt/oos/etc/ssl/certs/ca-certificates.crt

mkdir -p "$OOS_PERSIST_DIR/tmp" "$OOS_PERSIST_DIR/cache/system" \
  "$OOS_PERSIST_DIR/cache/system/gstreamer-1.0" \
  "$OOS_PERSIST_DIR/cache/webassembly-aot" \
  "$OOS_PERSIST_DIR/users/0/system/share"

chroot "$OOS_ROOTFS" /system/bin/sh -c \
  'exec /opt/oos/bin/oos "$@"' oos "$@" &
oos_pid=$!
echo "$oos_pid" > "$OOS_PID_FILE"

forward_signal() {
  kill -TERM "$oos_pid" 2>/dev/null || true
}
trap forward_signal HUP INT TERM

set +e
wait "$oos_pid"
result=$?
set -e
if [ "$(cat "$OOS_PID_FILE" 2>/dev/null || true)" = "$oos_pid" ]; then
  rm -f "$OOS_PID_FILE"
fi
trap - HUP INT TERM
exit "$result"

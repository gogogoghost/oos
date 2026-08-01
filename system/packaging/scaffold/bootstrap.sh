#!/system/bin/sh

# Shared bootstrap state. This file is sourced by init.sh, start.sh, and
# deinit.sh inside the device environment.

OOS_HOME=$(CDPATH= cd "$(dirname "$0")" && pwd)
OOS_ROOTFS="$OOS_HOME/rootfs"
OOS_PERSIST_DIR=${OOS_PERSIST_DIR:-/data/oos}
OOS_STATE_DIR="$OOS_PERSIST_DIR/.bootstrap"
OOS_PID_FILE="$OOS_STATE_DIR/oos.pid"
OOS_ACTIVE_RES_FILE="$OOS_STATE_DIR/active-res"

# Some stock KaiOS images ship BusyBox without installing every applet as a
# standalone command. Keep the bootstrap scripts usable on those systems.
if command -v busybox >/dev/null 2>&1; then
  OOS_BUSYBOX=$(command -v busybox)
  if ! command -v awk >/dev/null 2>&1; then
    awk() {
      "$OOS_BUSYBOX" awk "$@"
    }
  fi
  if ! command -v sha256sum >/dev/null 2>&1; then
    sha256sum() {
      "$OOS_BUSYBOX" sha256sum "$@"
    }
  fi
  # Android 6 toolbox mount lacks the --bind form used by the rootfs setup.
  mount() {
    "$OOS_BUSYBOX" mount "$@"
  }
  umount() {
    "$OOS_BUSYBOX" umount "$@"
  }
fi

if [ ! -f "$OOS_HOME/bootstrap.conf" ]; then
  echo "missing $OOS_HOME/bootstrap.conf" >&2
  exit 1
fi

# bootstrap.conf is part of the trusted, read-only scaffold package.
. "$OOS_HOME/bootstrap.conf"
if [ "${OOS_SCAFFOLD_FORMAT:-}" != 2 ]; then
  echo "unsupported OOS scaffold format: ${OOS_SCAFFOLD_FORMAT:-missing}" >&2
  exit 1
fi

is_mounted() {
  grep -q " $1 " /proc/mounts 2>/dev/null
}

require_directory() {
  if [ ! -d "$1" ]; then
    echo "required directory is missing: $1" >&2
    return 1
  fi
}

require_file() {
  if [ ! -f "$1" ]; then
    echo "required file is missing: $1" >&2
    return 1
  fi
}

resolve_res() {
  if [ ! -L "$OOS_HOME/res" ]; then
    echo "$OOS_HOME/res must be a symbolic link to a versioned res directory" >&2
    return 1
  fi
  OOS_RES_DIR=$(CDPATH= cd -P "$OOS_HOME/res" 2>/dev/null && pwd) || {
    echo "cannot resolve $OOS_HOME/res" >&2
    return 1
  }
  case "$OOS_RES_DIR" in
    "$OOS_HOME"/res-*) ;;
    *)
      echo "res target must be a versioned directory below $OOS_HOME" >&2
      return 1
      ;;
  esac
  require_file "$OOS_RES_DIR/manifest.env" || return 1
  require_file "$OOS_RES_DIR/COMPLETE" || return 1
  require_file "$OOS_RES_DIR/SHA256SUMS" || return 1
  require_file "$OOS_RES_DIR/bin/oos" || return 1
  require_file \
    "$OOS_RES_DIR/packages/org.orangeos.launcher/application.zip" || return 1
  if ! grep -q '^format=2$' "$OOS_RES_DIR/manifest.env" ||
      ! grep -q '^type=oos-res$' "$OOS_RES_DIR/manifest.env"; then
    echo "res package has an unsupported manifest format" >&2
    return 1
  fi
  expected_complete=$(cat "$OOS_RES_DIR/COMPLETE")
  actual_complete=$(sha256sum "$OOS_RES_DIR/SHA256SUMS" | awk '{print $1}')
  if [ "$expected_complete" != "$actual_complete" ]; then
    echo "res package completion marker is invalid" >&2
    return 1
  fi
  if ! grep -q "^device=$OOS_DEVICE$" "$OOS_RES_DIR/manifest.env"; then
    echo "res package does not target $OOS_DEVICE" >&2
    return 1
  fi
}

acquire_bootstrap_lock() {
  mkdir -p "$OOS_STATE_DIR"
  lock_dir="$OOS_STATE_DIR/lifecycle.lock"
  if [ -n "${OOS_LOCK_OWNER:-}" ] &&
      [ "$(cat "$lock_dir/pid" 2>/dev/null || true)" = "$OOS_LOCK_OWNER" ]; then
    OOS_LOCK_DIR=
    return 0
  fi
  if mkdir "$lock_dir" 2>/dev/null; then
    echo $$ > "$lock_dir/pid"
    OOS_LOCK_DIR="$lock_dir"
    OOS_LOCK_OWNER=$$
    export OOS_LOCK_OWNER
    return 0
  fi

  lock_pid=$(cat "$lock_dir/pid" 2>/dev/null || true)
  if [ -n "$lock_pid" ] && kill -0 "$lock_pid" 2>/dev/null; then
    echo "OOS lifecycle operation is already running with pid $lock_pid" >&2
    return 1
  fi
  rm -rf "$lock_dir"
  mkdir "$lock_dir"
  echo $$ > "$lock_dir/pid"
  OOS_LOCK_DIR="$lock_dir"
  OOS_LOCK_OWNER=$$
  export OOS_LOCK_OWNER
}

release_bootstrap_lock() {
  if [ -n "${OOS_LOCK_DIR:-}" ]; then
    rm -rf "$OOS_LOCK_DIR"
    OOS_LOCK_DIR=
    OOS_LOCK_OWNER=
    export OOS_LOCK_OWNER
  fi
}

oos_pid_running() {
  candidate_pid=$1
  [ -n "$candidate_pid" ] || return 1
  kill -0 "$candidate_pid" 2>/dev/null || return 1
  command_line=$(tr '\000' ' ' < "/proc/$candidate_pid/cmdline" 2>/dev/null || true)
  case "$command_line" in
    *"/opt/oos/bin/oos"*|*"/bin/oos"*) return 0 ;;
    *) return 1 ;;
  esac
}

path_is_mounted() {
  awk -v path="$1" '$2 == path { found=1 } END { exit !found }' \
    /proc/mounts 2>/dev/null
}

wait_for_service() {
  service_name=$1
  limit=${2:-50}
  attempt=0
  while [ "$(getprop "init.svc.$service_name")" != "running" ]; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge "$limit" ]; then
      echo "$service_name did not reach running state" >&2
      return 1
    fi
    sleep 0.1
  done
}

android_audio_calibration_ready() {
  media_pids=$(ps | awk \
    '$NF == "/system/bin/mediaserver" || $NF == "/system/bin/audioserver" { print $2 }')
  for media_pid in $media_pids; do
    process_dir="/proc/$media_pid"
    for descriptor in "$process_dir"/fd/*; do
      [ "$(readlink "$descriptor" 2>/dev/null || true)" = \
        /dev/msm_audio_cal ] && return 0
    done
  done
  return 1
}

wait_for_audio_runtime() {
  case "$1" in
    android6-qcom) ;;
    *)
      echo "unsupported audio readiness mode: $1" >&2
      return 1
      ;;
  esac

  attempt=0
  while ! service check media.audio_flinger 2>/dev/null | grep -q found ||
      ! android_audio_calibration_ready; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 150 ]; then
      echo "Android audio calibration did not become ready; keep b2g running or reboot after a failed media-service restart" >&2
      return 1
    fi
    sleep 0.1
  done

  # Service registration happens before mediaserver starts its Binder thread
  # pool. A bounded dump proves that OpenSL calls will actually be serviced.
  timeout_command=$(command -v timeout 2>/dev/null || true)
  if [ -z "$timeout_command" ]; then
    echo "audio readiness checks require a timeout command" >&2
    return 1
  fi
  binder_attempt=0
  while ! "$timeout_command" 3 /system/bin/dumpsys \
      media.audio_flinger >/dev/null 2>&1; do
    binder_attempt=$((binder_attempt + 1))
    if [ "$binder_attempt" -ge 3 ]; then
      echo "AudioFlinger is registered but not responding; keep b2g running until Android media initialization completes" >&2
      return 1
    fi
    sleep 0.5
  done
}

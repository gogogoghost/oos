#!/system/bin/sh

# Shared bootstrap state. This file is sourced by init.sh, start.sh, and
# deinit.sh inside the device environment.

OOS_HOME=$(CDPATH= cd "$(dirname "$0")" && pwd)
OOS_ROOTFS="$OOS_HOME/rootfs"
OOS_PERSIST_DIR=${OOS_PERSIST_DIR:-/data/oos}
OOS_STATE_DIR="$OOS_PERSIST_DIR/.bootstrap"
OOS_PID_FILE="$OOS_STATE_DIR/oos.pid"
OOS_ACTIVE_RES_FILE="$OOS_STATE_DIR/active-res"

if [ ! -f "$OOS_HOME/bootstrap.conf" ]; then
  echo "missing $OOS_HOME/bootstrap.conf" >&2
  exit 1
fi

# bootstrap.conf is part of the trusted, read-only scaffold package.
. "$OOS_HOME/bootstrap.conf"

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
  require_file "$OOS_RES_DIR/lib/libc++_shared.so" || return 1
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
  lock_dir="$OOS_STATE_DIR/$1.lock"
  if mkdir "$lock_dir" 2>/dev/null; then
    echo $$ > "$lock_dir/pid"
    OOS_LOCK_DIR="$lock_dir"
    return 0
  fi

  lock_pid=$(cat "$lock_dir/pid" 2>/dev/null || true)
  if [ -n "$lock_pid" ] && kill -0 "$lock_pid" 2>/dev/null; then
    echo "$1 is already running with pid $lock_pid" >&2
    return 1
  fi
  rm -rf "$lock_dir"
  mkdir "$lock_dir"
  echo $$ > "$lock_dir/pid"
  OOS_LOCK_DIR="$lock_dir"
}

release_bootstrap_lock() {
  if [ -n "${OOS_LOCK_DIR:-}" ]; then
    rm -rf "$OOS_LOCK_DIR"
    OOS_LOCK_DIR=
  fi
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

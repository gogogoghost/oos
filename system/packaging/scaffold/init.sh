#!/system/bin/sh

set -eu

. "$(CDPATH= cd "$(dirname "$0")" && pwd)/bootstrap.sh"

ADDED_MOUNTS=

record_mount() {
  ADDED_MOUNTS="$1 $ADDED_MOUNTS"
}

mount_bind() {
  source_path=$1
  target_path=$2
  require_directory "$source_path" || return 1
  require_directory "$target_path" || return 1
  if is_mounted "$target_path"; then
    return 0
  fi
  mount -o bind "$source_path" "$target_path"
  record_mount "$target_path"
}

mount_bind_optional() {
  source_path=$1
  target_path=$2
  if [ ! -d "$source_path" ]; then
    return 0
  fi
  mount_bind "$source_path" "$target_path"
}

mount_storage_optional() {
  source_path=$1
  target_path=$2
  [ -d "$source_path" ] || return 0
  mkdir -p "$target_path"
  mount_bind "$source_path" "$target_path"
}

mount_first_removable() {
  target_path=$1
  for source_path in ${OOS_REMOVABLE_STORAGE_CANDIDATES:-}; do
    if [ -d "$source_path" ] && path_is_mounted "$source_path"; then
      mount_storage_optional "$source_path" "$target_path"
      echo "$source_path" > "$OOS_STATE_DIR/removable-source"
      return 0
    fi
  done
  rm -f "$OOS_STATE_DIR/removable-source"
}

mount_filesystem() {
  filesystem=$1
  source_name=$2
  target_path=$3
  require_directory "$target_path" || return 1
  if is_mounted "$target_path"; then
    return 0
  fi
  mount -t "$filesystem" "$source_name" "$target_path"
  record_mount "$target_path"
}

rollback() {
  for target_path in $ADDED_MOUNTS; do
    umount "$target_path" 2>/dev/null || true
  done
}

acquire_bootstrap_lock init
trap 'rollback; release_bootstrap_lock' 0

resolve_res
require_directory "$OOS_ROOTFS"
mkdir -p "$OOS_PERSIST_DIR"
chmod 0700 "$OOS_PERSIST_DIR"
mkdir -p "$OOS_PERSIST_DIR/system" "$OOS_PERSIST_DIR/runtime" \
  "$OOS_PERSIST_DIR/packages" "$OOS_PERSIST_DIR/users/0/apps" \
  "$OOS_PERSIST_DIR/cache/apps" "$OOS_PERSIST_DIR/staging" \
  "$OOS_PERSIST_DIR/tmp" "$OOS_PERSIST_DIR/media/internal" \
  "$OOS_PERSIST_DIR/media/removable"

if is_mounted "$OOS_ROOTFS/opt/oos" &&
    [ -f "$OOS_ACTIVE_RES_FILE" ] &&
    [ "$(cat "$OOS_ACTIVE_RES_FILE")" != "$OOS_RES_DIR" ]; then
  echo "another res version is mounted; run deinit.sh before switching" >&2
  exit 1
fi

mount_bind /system "$OOS_ROOTFS/system"
mount_bind /dev "$OOS_ROOTFS/dev"
mount_filesystem proc proc "$OOS_ROOTFS/proc"
mount_filesystem sysfs sysfs "$OOS_ROOTFS/sys"
mount_bind_optional /vendor "$OOS_ROOTFS/vendor"
mount_bind_optional /apex/com.android.runtime \
  "$OOS_ROOTFS/apex/com.android.runtime"
mount_bind "$OOS_PERSIST_DIR" "$OOS_ROOTFS/data"
mkdir -p "$OOS_ROOTFS/data/misc/wifi" "$OOS_ROOTFS/data/vendor/wifi"
mount_bind_optional /data/misc/wifi "$OOS_ROOTFS/data/misc/wifi"
mount_bind_optional /data/vendor/wifi "$OOS_ROOTFS/data/vendor/wifi"
mount_bind "$OOS_RES_DIR" "$OOS_ROOTFS/opt/oos"
mount_storage_optional "${OOS_INTERNAL_STORAGE:-/storage/emulated/0}" \
  "$OOS_ROOTFS/data/media/internal"
mount_first_removable "$OOS_ROOTFS/data/media/removable"

echo "$OOS_RES_DIR" > "$OOS_ACTIVE_RES_FILE"
ADDED_MOUNTS=
release_bootstrap_lock
trap - 0
echo "OOS rootfs initialized with $(basename "$OOS_RES_DIR")"

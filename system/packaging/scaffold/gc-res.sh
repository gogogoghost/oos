#!/system/bin/sh

set -eu

. "$(CDPATH= cd "$(dirname "$0")" && pwd)/bootstrap.sh"

acquire_bootstrap_lock gc
trap 'release_bootstrap_lock' 0

active_res=$(readlink "$OOS_HOME/res" 2>/dev/null || true)
previous_res=$(cat "$OOS_STATE_DIR/previous-res" 2>/dev/null || true)
for res_path in "$OOS_HOME"/res-*; do
  [ -d "$res_path" ] || continue
  res_name=$(basename "$res_path")
  if [ "$res_name" = "$active_res" ] || [ "$res_name" = "$previous_res" ]; then
    continue
  fi
  if [ "${1:-}" = "--apply" ]; then
    rm -rf "$res_path"
    echo "Removed $res_name"
  else
    echo "Would remove $res_name"
  fi
done

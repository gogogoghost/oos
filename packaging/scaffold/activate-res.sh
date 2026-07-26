#!/system/bin/sh

set -eu

OOS_HOME=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$OOS_HOME/bootstrap.sh"

if [ "$#" -ne 1 ]; then
  echo "usage: $0 VERSION" >&2
  exit 2
fi

case "$1" in
  *[!0-9A-Za-z.+-]*)
    echo "invalid res version: $1" >&2
    exit 2
    ;;
  res-*) res_name=$1 ;;
  *) res_name="res-$1" ;;
esac
case "$res_name" in
  res-[0-9]*.[0-9]*.[0-9]*) ;;
  *)
    echo "invalid res version: $1" >&2
    exit 2
    ;;
esac

res_dir="$OOS_HOME/$res_name"
require_directory "$res_dir"
require_file "$res_dir/manifest.env"
require_file "$res_dir/SHA256SUMS"
require_file "$res_dir/COMPLETE"
if ! grep -q "^device=$OOS_DEVICE$" "$res_dir/manifest.env"; then
  echo "$res_name does not target $OOS_DEVICE" >&2
  exit 1
fi

if [ -f "$OOS_PID_FILE" ]; then
  oos_pid=$(cat "$OOS_PID_FILE" 2>/dev/null || true)
  if [ -n "$oos_pid" ] && kill -0 "$oos_pid" 2>/dev/null; then
    echo "stop OOS before activating another res version" >&2
    exit 1
  fi
fi
if is_mounted "$OOS_ROOTFS/opt/oos"; then
  echo "run deinit.sh before activating another res version" >&2
  exit 1
fi

expected_complete=$(cat "$res_dir/COMPLETE")
actual_complete=$(sha256sum "$res_dir/SHA256SUMS" | awk '{print $1}')
if [ "$expected_complete" != "$actual_complete" ]; then
  echo "$res_name has an invalid completion marker" >&2
  exit 1
fi
(
  cd "$res_dir"
  sha256sum -c SHA256SUMS
) >/dev/null

ln -sfn "$res_name" "$OOS_HOME/res"
if [ "$(readlink "$OOS_HOME/res")" != "$res_name" ]; then
  echo "failed to activate $res_name" >&2
  exit 1
fi
echo "Activated $res_name"

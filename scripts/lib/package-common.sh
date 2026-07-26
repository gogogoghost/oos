#!/usr/bin/env bash

package_die() {
  echo "$*" >&2
  exit 1
}

package_require_file() {
  [[ -f "$1" ]] || package_die "Missing required file: $1"
}

package_require_directory() {
  [[ -d "$1" ]] || package_die "Missing required directory: $1"
}

package_source_date_epoch() {
  if [[ -n "${SOURCE_DATE_EPOCH:-}" ]]; then
    echo "$SOURCE_DATE_EPOCH"
  else
    git -C "$ROOT_DIR" log -1 --format=%ct
  fi
}

package_create_tgz() {
  local source_dir=$1
  local archive=$2
  local source_parent source_name epoch
  source_parent=$(dirname "$source_dir")
  source_name=$(basename "$source_dir")
  epoch=$(package_source_date_epoch)
  mkdir -p "$(dirname "$archive")"
  tar --sort=name --format=gnu --owner=0 --group=0 --numeric-owner \
    --mtime="@$epoch" -czf "$archive" -C "$source_parent" "$source_name"
}

package_write_checksums() {
  local directory=$1
  (
    cd "$directory"
    find . -type f ! -name SHA256SUMS ! -name COMPLETE -print0 \
      | LC_ALL=C sort -z \
      | xargs -0 sha256sum
  ) > "$directory/SHA256SUMS"
  (
    cd "$directory"
    sha256sum SHA256SUMS | awk '{print $1}' > COMPLETE
  )
}

package_verify_elf_dependencies() {
  local runtime_dir=$1
  local system_dir=$2
  local work_dir needed available missing
  work_dir=$(mktemp -d "${TMPDIR:-/tmp}/oos-elf-deps.XXXXXX")
  needed="$work_dir/needed"
  available="$work_dir/available"
  missing="$work_dir/missing"

  find "$runtime_dir/bin" "$runtime_dir/lib" "$runtime_dir/libexec" \
    -type f -print0 \
    | while IFS= read -r -d '' runtime_file; do
        { readelf -d "$runtime_file" 2>/dev/null || true; } \
          | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p'
      done \
    | LC_ALL=C sort -u > "$needed"
  find "$runtime_dir/lib" "$system_dir/lib" \( -type f -o -type l \) \
    -printf '%f\n' \
    | LC_ALL=C sort -u > "$available"
  comm -23 "$needed" "$available" > "$missing"
  if [[ -s "$missing" ]]; then
    echo "Runtime package has unresolved shared-library dependencies:" >&2
    sed 's/^/  /' "$missing" >&2
    rm -rf "$work_dir"
    return 1
  fi
  echo "Verified $(wc -l < "$needed") runtime shared-library dependencies"
  rm -rf "$work_dir"
}

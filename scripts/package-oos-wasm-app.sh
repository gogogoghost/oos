#!/usr/bin/env bash

set -euo pipefail
umask 077

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MANIFEST=
WASM=
AOT=
ASSETS=
OUTPUT=
MODULES=()

usage() {
  echo "usage: $0 --manifest FILE [--wasm FILE] [--aot FILE] [--assets DIR] [--module NAME=FILE]... --output APPLICATION.zip" >&2
  echo "       at least one of --wasm or --aot is required" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --manifest) MANIFEST=${2:-}; shift 2 ;;
    --wasm) WASM=${2:-}; shift 2 ;;
    --aot) AOT=${2:-}; shift 2 ;;
    --assets) ASSETS=${2:-}; shift 2 ;;
    --module) MODULES+=("${2:-}"); shift 2 ;;
    --output) OUTPUT=${2:-}; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done

[[ -f "$MANIFEST" && -n "$OUTPUT" && ( -n "$WASM" || -n "$AOT" ) ]] || {
  usage
  exit 2
}
if [[ -n "$WASM" && ! -f "$WASM" ]]; then
  echo "Wasm module does not exist: $WASM" >&2
  exit 1
fi
if [[ -n "$AOT" && ! -f "$AOT" ]]; then
  echo "AOT module does not exist: $AOT" >&2
  exit 1
fi
if [[ -n "$ASSETS" && ! -d "$ASSETS" ]]; then
  echo "Asset directory does not exist: $ASSETS" >&2
  exit 1
fi
for module in "${MODULES[@]}"; do
  name=${module%%=*}
  path=${module#*=}
  if [[ "$name" == "$module" || ! "$name" =~ ^[A-Za-z0-9._-]+$ ||
        ! "$path" =~ \.(aot|wasm)$ || ! -f "$path" ]]; then
    echo "invalid child module (expected NAME=FILE.aot|wasm): $module" >&2
    exit 1
  fi
done
declare -A MODULE_DESTINATIONS=()
for module in "${MODULES[@]}"; do
  name=${module%%=*}
  path=${module#*=}
  destination="$name.${path##*.}"
  if [[ -n "${MODULE_DESTINATIONS[$destination]:-}" ]]; then
    echo "duplicate child module destination: modules/$destination" >&2
    exit 1
  fi
  MODULE_DESTINATIONS[$destination]=1
done
command -v zip >/dev/null || {
  echo "zip is required to package an OOS application" >&2
  exit 1
}

OUTPUT=$(realpath -m "$OUTPUT")
STAGING=$(mktemp -d "${TMPDIR:-/tmp}/oos-wasm-package.XXXXXX")
TEMPORARY="$OUTPUT.tmp.$$.zip"
trap 'rm -rf "$STAGING"; rm -f "$TEMPORARY"' EXIT
install -m 0644 "$MANIFEST" "$STAGING/manifest.json"
entries=(manifest.json)
if [[ -n "$WASM" ]]; then
  install -m 0644 "$WASM" "$STAGING/entry.wasm"
  entries+=(entry.wasm)
fi
if [[ -n "$AOT" ]]; then
  install -m 0644 "$AOT" "$STAGING/entry.aot"
  entries+=(entry.aot)
fi
if (( ${#MODULES[@]} )); then
  mkdir -p "$STAGING/modules"
  for module in "${MODULES[@]}"; do
    name=${module%%=*}
    path=${module#*=}
    extension=${path##*.}
    destination="modules/$name.$extension"
    install -m 0644 "$path" "$STAGING/$destination"
    entries+=("$destination")
  done
fi
if [[ -n "$ASSETS" ]]; then
  mkdir -p "$STAGING/assets"
  cp -a "$ASSETS/." "$STAGING/assets/"
  while IFS= read -r -d '' asset; do
    relative=${asset#"$STAGING/"}
    entries+=("$relative")
  done < <(find "$STAGING/assets" -type f -print0 | sort -z)
fi
PACKAGE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$ROOT_DIR" log -1 --format=%ct)}
for entry in "${entries[@]}"; do
  touch -d "@$PACKAGE_EPOCH" "$STAGING/$entry"
done
mkdir -p "$(dirname "$OUTPUT")"
(cd "$STAGING" && zip -X -q -D -9 "$TEMPORARY" "${entries[@]}")
mv -f "$TEMPORARY" "$OUTPUT"
echo "Created $OUTPUT"

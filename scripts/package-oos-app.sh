#!/usr/bin/env bash

set -euo pipefail
umask 077

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MANIFEST=
JS=
WASM=
AOTS=()
APP_DIR=
ASSETS=
OUTPUT=
MODULES=()

usage() {
  echo "usage: $0 --manifest FILE [--js FILE | [--wasm FILE] [--aot TARGET=FILE]...] [--app-dir DIR] [--assets DIR] [--module NAME=FILE]... --output APPLICATION.zip" >&2
  echo "       exactly one runtime entry is required; Wasm packages may contain portable and target AOT artifacts" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --manifest) MANIFEST=${2:-}; shift 2 ;;
    --js) JS=${2:-}; shift 2 ;;
    --wasm) WASM=${2:-}; shift 2 ;;
    --aot) AOTS+=("${2:-}"); shift 2 ;;
    --app-dir) APP_DIR=${2:-}; shift 2 ;;
    --assets) ASSETS=${2:-}; shift 2 ;;
    --module) MODULES+=("${2:-}"); shift 2 ;;
    --output) OUTPUT=${2:-}; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done

if [[ ! -f "$MANIFEST" || -z "$OUTPUT" ]] ||
    ! { [[ -n "$JS" && -z "$WASM" && ${#AOTS[@]} -eq 0 ]] ||
        [[ -z "$JS" && ( -n "$WASM" || ${#AOTS[@]} -gt 0 ) ]]; }; then
  usage
  exit 2
fi
if [[ -n "$JS" && ! -f "$JS" ]]; then
  echo "JavaScript module does not exist: $JS" >&2
  exit 1
fi
if [[ -n "$WASM" && ! -f "$WASM" ]]; then
  echo "Wasm module does not exist: $WASM" >&2
  exit 1
fi
declare -A AOT_TARGETS=()
for artifact in "${AOTS[@]}"; do
  target=${artifact%%=*}
  path=${artifact#*=}
  if [[ "$target" == "$artifact" ||
        ${#target} -gt 64 ||
        ! "$target" =~ ^[a-z0-9]([a-z0-9_-]*[a-z0-9])?$ ||
        ! -f "$path" ]]; then
    echo "invalid AOT artifact (expected TARGET=FILE): $artifact" >&2
    exit 1
  fi
  if [[ -n "${AOT_TARGETS[$target]:-}" ]]; then
    echo "duplicate application AOT target: $target" >&2
    exit 1
  fi
  AOT_TARGETS[$target]=1
done
if [[ -n "$ASSETS" && ! -d "$ASSETS" ]]; then
  echo "Asset directory does not exist: $ASSETS" >&2
  exit 1
fi
if [[ -n "$APP_DIR" && ! -d "$APP_DIR" ]]; then
  echo "Application source directory does not exist: $APP_DIR" >&2
  exit 1
fi
for module in "${MODULES[@]}"; do
  name=${module%%=*}
  path=${module#*=}
  if [[ "$name" == "$module" || ! "$name" =~ ^[A-Za-z0-9._-]+$ ||
        ! "$path" =~ \.(aot|wasm|js|mjs)$ || ! -f "$path" ]]; then
    echo "invalid module (expected NAME=FILE.aot|wasm|js|mjs): $module" >&2
    exit 1
  fi
done
moduleDestination() {
  local name=$1
  local path=$2
  local filename stem target
  if [[ "$path" == *.aot ]]; then
    filename=${path##*/}
    stem=${filename%.aot}
    target=${stem##*.}
    if [[ ${#target} -gt 64 ||
          ! "$target" =~ ^[a-z0-9]([a-z0-9_-]*[a-z0-9])?$ ]]; then
      return 1
    fi
    echo "$name.$target.aot"
  else
    echo "$name.${path##*.}"
  fi
}
declare -A MODULE_DESTINATIONS=()
for module in "${MODULES[@]}"; do
  name=${module%%=*}
  path=${module#*=}
  if ! destination=$(moduleDestination "$name" "$path"); then
    echo "AOT module filename must end in .TARGET.aot: $path" >&2
    exit 1
  fi
  if [[ -n "${MODULE_DESTINATIONS[$destination]:-}" ]]; then
    echo "duplicate package module destination: modules/$destination" >&2
    exit 1
  fi
  MODULE_DESTINATIONS[$destination]=1
done
command -v zip >/dev/null || {
  echo "zip is required to package an OOS application" >&2
  exit 1
}

OUTPUT=$(realpath -m "$OUTPUT")
STAGING=$(mktemp -d "${TMPDIR:-/tmp}/oos-package.XXXXXX")
TEMPORARY="$OUTPUT.tmp.$$.zip"
trap 'rm -rf "$STAGING"; rm -f "$TEMPORARY"' EXIT
install -m 0644 "$MANIFEST" "$STAGING/manifest.json"
entries=(manifest.json)
mkdir -p "$STAGING/app"
if [[ -n "$APP_DIR" ]]; then
  cp -a "$APP_DIR/." "$STAGING/app/"
fi
if [[ -n "$JS" ]]; then
  install -m 0644 "$JS" "$STAGING/app/main.mjs"
fi
if [[ -n "$WASM" ]]; then
  install -m 0644 "$WASM" "$STAGING/app/main.wasm"
fi
for artifact in "${AOTS[@]}"; do
  target=${artifact%%=*}
  path=${artifact#*=}
  install -m 0644 "$path" "$STAGING/app/main.$target.aot"
done
while IFS= read -r -d '' source; do
  entries+=("${source#"$STAGING/"}")
done < <(find "$STAGING/app" -type f -print0 | sort -z)
if (( ${#MODULES[@]} )); then
  mkdir -p "$STAGING/modules"
  for module in "${MODULES[@]}"; do
    name=${module%%=*}
    path=${module#*=}
    destination="modules/$(moduleDestination "$name" "$path")"
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

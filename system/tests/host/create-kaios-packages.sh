#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 KAIOS25.zip KAIOS3.zip" >&2
  exit 2
fi
kaios25=$(realpath -m "$1")
kaios3=$(realpath -m "$2")
staging=$(mktemp -d "${TMPDIR:-/tmp}/oos-kaios-test.XXXXXX")
trap 'rm -rf "$staging"' EXIT

mkdir -p "$staging/kaios25" "$staging/kaios3"
printf '%s\n' \
  '{"name":"APN Config","version":"1.0.0","launch_path":"/index.html"}' \
  > "$staging/kaios25/manifest.webapp"
printf '%s\n' '<!doctype html><title>KaiOS 2.5</title>' \
  > "$staging/kaios25/index.html"
printf '%s\n' \
  '{"name":"Calculator","start_url":"./main.html?source=test","b2g_features":{"version":"3.0.0"}}' \
  > "$staging/kaios3/manifest.webmanifest"
printf '%s\n' '<!doctype html><title>KaiOS 3</title>' \
  > "$staging/kaios3/main.html"

mkdir -p "$(dirname "$kaios25")" "$(dirname "$kaios3")"
(cd "$staging/kaios25" && zip -q -D -9 "$kaios25" manifest.webapp index.html)
(cd "$staging/kaios3" && zip -q -D -9 "$kaios3" manifest.webmanifest main.html)

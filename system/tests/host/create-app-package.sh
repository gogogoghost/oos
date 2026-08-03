#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 || ! ${2:-aot} =~ ^(aot|wasm|both)$ ]]; then
  echo "usage: $0 OUTPUT.zip [aot|wasm|both]" >&2
  exit 2
fi
output=$(realpath -m "$1")
mode=${2:-aot}
staging=$(mktemp -d "${TMPDIR:-/tmp}/oos-test-package.XXXXXX")
trap 'rm -rf "$staging"' EXIT
entries=(manifest.json)
mkdir -p "$staging/assets/audio"
printf 'OOS packaged asset\n' > "$staging/assets/audio/test.dat"
entries+=(assets/audio/test.dat)
if [[ $mode == aot || $mode == both ]]; then
  printf '\0aot-test-module\n' > "$staging/entry.aot"
  entries+=(entry.aot)
fi
if [[ $mode == wasm || $mode == both ]]; then
  printf '\0wasm-test-module\n' > "$staging/entry.wasm"
  entries+=(entry.wasm)
fi
cat > "$staging/manifest.json" <<'EOF'
{
  "id": "cc.jaxy.oos.test",
  "name": "OOS Test",
  "version": "1.0.0",
  "role": "test",
  "permissions": {"camera": {}, "wifi-manage": {}}
}
EOF
mkdir -p "$(dirname "$output")"
(cd "$staging" && zip -q -D -9 "$output" "${entries[@]}")

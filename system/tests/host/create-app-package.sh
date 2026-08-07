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
mkdir -p "$staging/modules"
mkdir -p "$staging/app"
printf 'OOS packaged asset\n' > "$staging/assets/audio/test.dat"
printf '\0child-wasm-test-module\n' > "$staging/modules/compiler.wasm"
printf '\0child-aot-test-module\n' > "$staging/modules/runtime.cortex-a53.aot"
entries+=(assets/audio/test.dat)
entries+=(modules/compiler.wasm modules/runtime.cortex-a53.aot)
if [[ $mode == aot || $mode == both ]]; then
  printf '\0aot-test-module\n' > "$staging/app/main.cortex-a53.aot"
  entries+=(app/main.cortex-a53.aot)
fi
if [[ $mode == wasm || $mode == both ]]; then
  printf '\0wasm-test-module\n' > "$staging/app/main.wasm"
  entries+=(app/main.wasm)
fi
cat > "$staging/manifest.json" <<EOF
{
  "schema": 1,
  "id": "cc.jaxy.oos.test",
  "name": "OOS Test",
  "version": "1.0.0",
  "role": "test",
  "entry": {"runtime": "wasm", "path": "app/main"},
  "modules": [
    {"name": "compiler", "runtime": "wasm", "path": "modules/compiler"},
    {"name": "runtime", "runtime": "wasm", "path": "modules/runtime"}
  ],
  "permissions": {"camera": {}, "wifi-manage": {}},
  "handlers": [{"kind": "activity", "value": "test"}]
}
EOF
mkdir -p "$(dirname "$output")"
(cd "$staging" && zip -q -D -9 "$output" "${entries[@]}")

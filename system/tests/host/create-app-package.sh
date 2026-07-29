#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 OUTPUT.zip" >&2
  exit 2
fi
output=$(realpath -m "$1")
staging=$(mktemp -d "${TMPDIR:-/tmp}/oos-test-package.XXXXXX")
trap 'rm -rf "$staging"' EXIT
mkdir -p "$staging/aot/armv7/wamr-2.4.4"
printf '\0aot-test-module\n' > "$staging/aot/armv7/wamr-2.4.4/app.aot"
cat > "$staging/oos-manifest.json" <<'EOF'
{
  "format": 1,
  "id": "org.orangeos.test",
  "name": "OOS Test",
  "version": "1.0.0",
  "package_kind": "oos-wasm-v1",
  "runtime_kind": "wamr",
  "api_profile": "oos-wit-0.1",
  "entrypoint": "aot/armv7/wamr-2.4.4/app.aot",
  "role": "test",
  "permissions": {"camera": {}, "wifi-manage": {}},
  "memory": {"stack_bytes": 131072, "heap_bytes": 4194304}
}
EOF
mkdir -p "$(dirname "$output")"
(cd "$staging" && zip -q -D -9 "$output" oos-manifest.json \
  aot/armv7/wamr-2.4.4/app.aot)

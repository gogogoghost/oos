#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
LAUNCHER_CORE=${1:-"$ROOT_DIR/build/native-apps/launcher.wasm"}
LAUNCHER_COMPONENT=${2:-"$ROOT_DIR/build/native-apps/launcher.component.wasm"}
SMOKE_CORE=${3:-"$ROOT_DIR/build/native-apps/wit-smoke.wasm"}

for tool in wasm-tools rg; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "$tool is required to verify WIT interfaces." >&2
    exit 1
  }
done
for artifact in "$LAUNCHER_CORE" "$LAUNCHER_COMPONENT" "$SMOKE_CORE"; do
  [[ -f "$artifact" ]] || {
    echo "Missing WIT artifact: $artifact" >&2
    exit 1
  }
done

wasm-tools component wit "$ROOT_DIR/apps/sdk/wit/oos.wit" --json >/dev/null
wasm-tools validate "$LAUNCHER_CORE"
wasm-tools validate "$LAUNCHER_COMPONENT"
wasm-tools validate "$SMOKE_CORE"

TEMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/oos-wit-verify.XXXXXX")
cleanup() {
  rm -rf "$TEMP_DIR"
}
trap cleanup EXIT

wasm-tools print "$LAUNCHER_CORE" >"$TEMP_DIR/launcher.wat"
wasm-tools print "$SMOKE_CORE" >"$TEMP_DIR/smoke.wat"
wasm-tools component wit "$LAUNCHER_COMPONENT" >"$TEMP_DIR/component.wit"

for import in runtime graphics; do
  rg -Fq "(import \"oos:platform/$import@0.1.0\"" \
    "$TEMP_DIR/launcher.wat"
done
for export in init event frame shutdown; do
  rg -Fq "(export \"oos:platform/lifecycle@0.1.0#$export\"" \
    "$TEMP_DIR/launcher.wat"
done
if rg -q '\(import "oos" "oos_' "$TEMP_DIR/launcher.wat"; then
  echo "Launcher still imports the legacy OOS ABI." >&2
  exit 1
fi

for interface in \
  runtime graphics device audio camera power vibrator wifi ip bluetooth modem codec \
  storage device-storage system-services; do
  rg -Fq "(import \"oos:platform/$interface@0.1.0\"" \
    "$TEMP_DIR/smoke.wat"
done
rg -Fq 'export oos:platform/lifecycle@0.1.0;' \
  "$TEMP_DIR/component.wit"

echo "Verified OOS WIT core and component interfaces."

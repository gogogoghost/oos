#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
LAUNCHER_DIR="$ROOT_DIR/launcher"

if ! command -v bun >/dev/null 2>&1; then
  echo "Bun is required to build the Solid.js launcher." >&2
  exit 1
fi

(
  cd "$LAUNCHER_DIR"
  bun install --frozen-lockfile
  bun run build
)

test -f "$LAUNCHER_DIR/dist/index.html"
mapfile -t dist_files < <(
  find "$LAUNCHER_DIR/dist" -type f -printf '%P\n' | sort
)
if [[ ${#dist_files[@]} -ne 1 || "${dist_files[0]}" != index.html ]]; then
  echo "Launcher production build must be a single index.html file." >&2
  printf '  %s\n' "${dist_files[@]}" >&2
  exit 1
fi
grep -q '<style' "$LAUNCHER_DIR/dist/index.html"
grep -q '<script[^>]*>[^<]' "$LAUNCHER_DIR/dist/index.html"
grep -q 'data:image/svg+xml' "$LAUNCHER_DIR/dist/index.html"
echo "Built launcher at $LAUNCHER_DIR/dist"

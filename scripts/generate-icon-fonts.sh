#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT_DIR"
SOURCE="third_party/lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff"
RANGES='61451,61459,61461,61468,61502,61524,61550,61553,61563,61589,61664,61671,61787,61931,62016-62020,62099,63426'

[[ -f "$SOURCE" ]] || {
  echo "FontAwesome source is missing: $SOURCE" >&2
  exit 1
}

for font_size in 12 14 20; do
  output="sdk/cpp/src/oos_icon_font_${font_size}.c"
  npx --yes lv_font_conv@1.5.3 \
    --size "$font_size" --bpp 4 --format lvgl \
    --font "$SOURCE" -r "$RANGES" \
    --no-kerning --no-compress \
    --lv-font-name "oos_icon_font_${font_size}" \
    -o "$output"
  LC_ALL=C sed -Ei 's@(\/\* U\+[0-9A-F]+) ".*" \*/@\1 */@' "$output"
done

echo "Generated OOS icon-only LVGL fonts"

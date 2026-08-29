#!/usr/bin/env bash
# Regenerate the AI Watch CJK font (ai_watch_font_cjk_16.c).
#
# Reproducible conversion, mirrors app/ai_watch/icons/convert_icons.py:
#   ASCII   : DejaVuSans.ttf (ships with the LVGL tree)   -r 0x20-0x7E
#   CJK     : cjk_charset.txt (GB2312 level-1 + full-width punctuation)
#             from DroidSansFallbackFull.ttf
#   output  : ai_watch_font_cjk_16.c (16 px, 2 bpp, no compression)
#
# NOTE: Droid Sans Fallback has NO basic-Latin glyphs (digits, letters,
# most ASCII punctuation).  It is the sole CJK source here; every ASCII
# range must come from a Latin font such as DejaVu.  lv_font_conv only
# errors on a requested range when the range is completely empty, so a
# wrong font/codec pairing fails silently — always verify the result
# (digits in the sparse unicode_list) after regenerating.
#
# Prerequisites: node/npm, then once:  npm install lv_font_conv
# (lv_font_conv converts to an LVGL v8/v9 compatible C font.)
#
# Usage: [LVGL_DIR=/path/to/lvgl] ./gen_cjk_font.sh

set -euo pipefail
cd "$(dirname "$0")"

LVGL_DIR="${LVGL_DIR:-/home/ma/openvela-contest403/apps/graphics/lvgl/lvgl}"
DEJAVU="$LVGL_DIR/scripts/built_in_font/DejaVuSans.ttf"
DROID=/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf

npx lv_font_conv \
  --no-compress --no-prefilter --size 16 --bpp 2 \
  --font "$DEJAVU" \
    -r 0x20-0x7E \
  --font "$DROID" \
    --symbols "$(cat cjk_charset.txt)" \
  --format lvgl --lv-font-name ai_watch_font_cjk_16 \
  -o ai_watch_font_cjk_16.c

echo "wrote $(pwd)/ai_watch_font_cjk_16.c"

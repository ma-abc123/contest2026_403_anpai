#!/usr/bin/env bash
# Regenerate the AI Watch CJK fonts.
#
# Reproducible conversion, mirrors app/ai_watch/icons/convert_icons.py:
#   ASCII   : DejaVuSans.ttf (ships with the LVGL tree)   -r 0x20-0x7E
#   CJK     : cjk_charset.txt (GB2312 level-1 + full-width punctuation)
#             from DroidSansFallbackFull.ttf
#   output  : ai_watch_font_cjk_16.c (16 px, 2 bpp, no compression)
#             ai_watch_font_weather_28.c (28 px, weather card only:
#             ASCII + the 7 weather words + ℃ -- a full charset at
#             28 px would cost ~1 MB of flash, this is a few KB)
#
# NOTE: Droid Sans Fallback has NO basic-Latin glyphs (digits, letters,
# most ASCII punctuation).  It is the sole CJK source here; every ASCII
# range must come from a Latin font such as DejaVu.  It also LACKS
# U+00B0 (°) and U+2103 (℃) even though cjk_charset.txt lists them --
# lv_font_conv drops such codepoints silently (it only errors on a
# completely empty range), which is how the degree ring vanished from
# the weather line.  Both are requested from DejaVu explicitly below,
# and MUST be stripped from the Droid symbol list: a codepoint listed
# for two fonts resolves from the LAST one, and a missing glyph there
# drops it entirely (observed: -r 0xB0 on DejaVu alone worked; adding
# the charset -- which lists °℃ -- to Droid un-did it).
# Strip with python, NOT tr: tr deletes BYTES, and the UTF-8 encodings
# of °℃ share bytes with ordinary Han characters (数 ends in 0xB0,
# 评 contains 0x84) -- tr silently corrupted half the charset.
# Always verify the result (mapped ranges in the cmaps[]) after
# regenerating.
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
    -r 0x20-0x7E -r 0xB0 -r 0x2103 \
  --font "$DROID" \
    --symbols "$(python3 -c "import sys; sys.stdout.write(open('cjk_charset.txt', encoding='utf-8').read().replace('°', '').replace('℃', ''))")" \
  --format lvgl --lv-font-name ai_watch_font_cjk_16 \
  -o ai_watch_font_cjk_16.c

echo "wrote $(pwd)/ai_watch_font_cjk_16.c"

# Weather card font: exactly the words ai_watch_weather_text() can render,
# plus the single-glyph ℃ (U+2103, from Droid) -- "%d°C" at 16 px had an
# invisible degree ring and was too small overall. Keep this symbol list
# in sync with ai_watch_weather_text().

npx lv_font_conv \
  --no-compress --no-prefilter --size 28 --bpp 2 \
  --font "$DEJAVU" \
    -r 0x20-0x7E -r 0xB0 -r 0x2103 \
  --font "$DROID" \
    --symbols "晴多云阴雨雪雾" \
  --format lvgl --lv-font-name ai_watch_font_weather_28 \
  -o ai_watch_font_weather_28.c

echo "wrote $(pwd)/ai_watch_font_weather_28.c"

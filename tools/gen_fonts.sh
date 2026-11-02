#!/usr/bin/env bash
# Regenerates the LVGL bitmap fonts in firmware/src/ui/fonts/.
#
# Inputs (not committed, download once):
#   Inter 4.1 (SIL OFL 1.1)      https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
#   LVGL FontAwesome subset      https://raw.githubusercontent.com/lvgl/lvgl/v8.3.11/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff
#
# usage: tools/gen_fonts.sh <dir containing Inter-*.ttf and FontAwesome5-Solid+Brands+Regular.woff>
set -euo pipefail
SRC=${1:?font source dir}
OUT=$(cd "$(dirname "$0")/.." && pwd)/firmware/src/ui/fonts
mkdir -p "$OUT"

# Basic Latin, Latin-1 Supplement, Latin Extended-A (all Czech/Slovak/Polish/German letters),
# typographic dashes/quotes, bullet, ellipsis, euro, arrows.
TEXT="0x20-0x7E,0xA0-0x17F,0x2013-0x2014,0x2018-0x201E,0x2022,0x2026,0x20AC,0x2190-0x2193"
# LV_SYMBOL_* code points used by the UI (wifi, batteries, bolt, envelope, bell, gear, check,
# warning, close, refresh, home, list, chevrons, eye, cloud, usb, comment, user)
ICONS="0xF1EB,0xF240-0xF244,0xF0E7,0xF0E0,0xF0F3,0xF013,0xF00C,0xF071,0xF00D,0xF021,0xF015,0xF00B,0xF053,0xF054,0xF077,0xF078,0xF06E,0xF0C2,0xF287,0xF075,0xF007"
FA="$SRC/FontAwesome5-Solid+Brands+Regular.woff"
conv() { npx -y lv_font_conv@1.5.2 --no-compress --no-prefilter --bpp 4 --format lvgl "$@"; }

conv --size 12 -o "$OUT/sp_font_12.c" \
     --font "$SRC/Inter-Medium.ttf" -r "$TEXT" --font "$FA" -r "$ICONS"
conv --size 14 -o "$OUT/sp_font_14.c" \
     --font "$SRC/Inter-Regular.ttf" -r "$TEXT" --font "$FA" -r "$ICONS"
conv --size 16 -o "$OUT/sp_font_16.c" \
     --font "$SRC/Inter-SemiBold.ttf" -r "$TEXT" --font "$FA" -r "$ICONS"
conv --size 22 -o "$OUT/sp_font_22.c" \
     --font "$SRC/Inter-Bold.ttf" -r "$TEXT"
conv --size 52 -o "$OUT/sp_font_clock.c" \
     --font "$SRC/Inter-SemiBold.ttf" -r "0x30-0x3A"
echo "fonts written to $OUT"

#!/bin/sh
# weather-station fork: the weather station's own fonts, made like
# upstream's (tools/gen_fonts.sh): Montserrat Medium, lv_font_conv 1.5.3.
#  - font_ws_digits_100: the weather view's current temperatures: digits,
#    ". : - /" and the minus sign U+2212.
#  - font_ws_digits_110: the screensaver's temperatures, the same glyphs. The
#    outdoor one reaches the radar's ring from about -10 °C (accepted).
#  - font_ws_digits_94: the screensaver's clock, digits and ":" only. It
#    reaches the ring at ~9 % of the day's minutes (wide times like "00:00",
#    the user's choice 2026-11-18; 88 px never did, 100 px at 38 %).
#  - font_ws_26: the screensaver's lines and the weather view's details:
#    printable ASCII, the Polish letters, the degree sign and middle dot,
#    LVGL's up/down arrows (trends), FontAwesome's weather icons and the
#    places' icons (tree: outdoor, house: indoor).
# Run from anywhere (Git Bash): sh tools/gen_ws_fonts.sh
set -e
cd "$(dirname "$0")/../main"
TTF=../managed_components/lvgl__lvgl/scripts/built_in_font/Montserrat-Medium.ttf
SYM=../managed_components/lvgl__lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff
# Ą ą Ć ć Ę ę Ł ł Ń ń Ó ó Ś ś Ź ź Ż ż, ° ·
PL=0x104,0x105,0x106,0x107,0x118,0x119,0x141,0x142,0x143,0x144,0xD3,0xF3,0x15A,0x15B,0x179,0x17A,0x17B,0x17C,0xB0,0xB7
# up, down (trends); sun, cloud-sun, cloud, smog, cloud-rain, cloud-showers-heavy, snowflake, bolt, wind;
# tree, house (the places)
ICONS=61559,61560,0xf185,0xf6c4,0xf0c2,0xf75f,0xf73d,0xf740,0xf2dc,0xf0e7,0xf72e,0xf1bb,0xf015

npx --yes lv_font_conv@1.5.3 --bpp 4 --size 100 \
    --font "$TTF" -r 0x20,0x2D-0x3A,0x2212 \
    --format lvgl --no-compress -o font_ws_digits_100.c

npx --yes lv_font_conv@1.5.3 --bpp 4 --size 110 \
    --font "$TTF" -r 0x20,0x2D-0x3A,0x2212 \
    --format lvgl --no-compress -o font_ws_digits_110.c

npx --yes lv_font_conv@1.5.3 --bpp 4 --size 94 \
    --font "$TTF" -r 0x20,0x30-0x3A \
    --format lvgl --no-compress -o font_ws_digits_94.c

npx --yes lv_font_conv@1.5.3 --bpp 4 --size 26 \
    --font "$TTF" -r 0x20-0x7E,"$PL" \
    --font "$SYM" -r "$ICONS" \
    --format lvgl --no-compress -o font_ws_26.c

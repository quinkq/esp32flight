#pragma once

/* weather-station fork: temperatures as the weather station shows them
 * (ui_weather_temp.c), for the weather view and the screensaver. LVGL lock
 * held for all of it. */

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

/* our fonts, made by tools/gen_ws_fonts.sh */
LV_FONT_DECLARE(font_ws_digits_100);
LV_FONT_DECLARE(font_ws_26);

/* The colour of a temperature in tenths of °C: violet at -50 °C, whitish at
 * 0, amber at 20, red at 50 (docs\mockups\temp-scale.png, decided
 * 2026-11-16); the same hues darker on a light theme. */
lv_color_t ui_temp_colour(int32_t tenths);

/* A large temperature: the whole degrees in the 100 px digits, the decimal,
 * the degree sign and an optional suffix (a trend arrow) in Montserrat 44,
 * on one baseline. Sized to its content. */
lv_obj_t *ui_temp_big_create(lv_obj_t *parent);

/* have false: a dash. suffix may be NULL. */
void ui_temp_big_set(lv_obj_t *big, bool have, int32_t tenths, lv_color_t colour, const char *suffix);

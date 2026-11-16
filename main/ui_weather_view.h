#pragma once

#include <stdbool.h>
#include "lvgl.h"

/* weather-station fork: the weather view, the right panel's sixth view
 * (ui_weather_view.c). ui.c builds it with the other panels and shows it from
 * apply_view(); all three need the LVGL lock. */

/* The panel at x, y, w × h on scr, hidden until shown. */
void ui_weather_view_build(lv_obj_t *scr, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h);

/* Shown or hidden; it refreshes itself every minute while shown. */
void ui_weather_view_show(bool shown);

/* In ui.c: the right panel to the weather view (taps on the screensaver's and
 * the header's weather fields). */
void ui_show_weather_view(void);

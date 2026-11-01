#pragma once

#include "lvgl.h"

/* weather-station fork: the weather station's touchscreen UI. */

/* The settings tab's name in the display's language. */
const char *ui_weather_tab_name(void);

/* Fills the settings tab: the outdoor unit's diagnostics, refreshed every
 * 5 s until the page is deleted (settings closed). LVGL lock held. */
void ui_weather_settings_tab(lv_obj_t *page);

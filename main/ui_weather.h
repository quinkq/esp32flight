#pragma once

#include "lvgl.h"

/* weather-station fork: the weather station's touchscreen UI. */

/* The settings tab's name in the display's language. */
const char *ui_weather_tab_name(void);

/* Fills the settings tab: the outdoor unit's diagnostics, refreshed every
 * 5 s until the page is deleted (settings closed). LVGL lock held. */
void ui_weather_settings_tab(lv_obj_t *page);

/* The settings overlay's top bar, once its title and Save button exist:
 * upstream's Save becomes "Save settings", "Save weather data" goes to its
 * left, and the update notice (NULL: none) moves into the space between
 * that and the title. LVGL lock held. */
void ui_weather_settings_header(lv_obj_t *overlay, lv_obj_t *title, lv_obj_t *save_btn, lv_obj_t *notice);

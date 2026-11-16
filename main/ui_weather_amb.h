#pragma once

/* weather-station fork: the weather station on the screensaver and in the
 * header (ui_weather_amb.c). Hooks in upstream's ui.c and flight_task.c. */

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"
#include "weather.h"

/* The screensaver (map style) was just built: its clock, Open-Meteo label
 * and map credit are upstream's; ours go on top. LVGL lock held. */
void ui_weather_amb_build(lv_obj_t *amb, lv_obj_t *clock, lv_obj_t *wx, lv_obj_t *attr);

/* A tap on the screensaver at pt: true if it hit a weather field (the
 * caller closes the screensaver and opens the weather view). */
bool ui_weather_amb_hit(const lv_point_t *pt);

/* The header's weather label, once: tapping it opens the weather view, and
 * it shows our temperatures from now on. LVGL lock held. */
void ui_weather_header_hook(lv_obj_t *label);

/* Open-Meteo's latest answer (flight_task.c, its task): kept for the
 * screensaver's words and wind, and text, upstream's header line, replaced
 * by ours when the weather station has something to show. */
void ui_weather_open_meteo(const weather_t *wx, char *text, size_t n);

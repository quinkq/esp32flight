#pragma once

/* weather-station fork: temperatures and current readings as the weather
 * station shows them (ui_weather_temp.c), for the weather view, the
 * screensaver and the header. LVGL lock held for the widgets. */

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "readings_store.h"

/* our fonts, made by tools/gen_ws_fonts.sh */
LV_FONT_DECLARE(font_ws_digits_100);
LV_FONT_DECLARE(font_ws_digits_88);
LV_FONT_DECLARE(font_ws_26);

/* The colour of a temperature in tenths of °C: violet at -50 °C, whitish at
 * 0, amber at 20, red at 50 (docs\mockups\temp-scale.png, decided
 * 2026-11-16); the same hues darker on a light theme. */
lv_color_t ui_temp_colour(int32_t tenths);

/* "#rrggbb" of that colour, for label recolouring */
void ui_temp_colour_code(char *buf, size_t n, int32_t tenths);

/* A large temperature: the whole degrees in the 100 px digits, the decimal,
 * the degree sign and an optional suffix (a trend arrow) in Montserrat 44,
 * on one baseline. Its box hugs the digits (no empty line space above or
 * below them), so a frame around it sits tight. */
lv_obj_t *ui_temp_big_create(lv_obj_t *parent);

/* have false: a dash. suffix may be NULL. */
void ui_temp_big_set(lv_obj_t *big, bool have, int32_t tenths, lv_color_t colour, const char *suffix);

/* Where a font's digits start below a label's top, and how tall they are:
 * for boxes that hug large digits. */
void ui_digits_metrics(const lv_font_t *font, lv_coord_t *top, lv_coord_t *height);

/* The outdoor temperature of a calibrated slot, tenths: the SHT40's, the
 * BME280's as the backup; false: none. */
bool ui_weather_out_temp(const readings_slot_t *s, int32_t *t);

/* The weather station now, calibrated, values in tenths (°C, %, hPa). */
typedef struct {
    bool     out_any;                 /* a stored outdoor reading at all */
    bool     out_stale;               /* silent, or (after a display restart) its newest reading too old */
    bool     out_have_t, out_have_rh, out_have_p;
    int32_t  out_t, out_rh, out_p;
    uint32_t out_ts;                  /* the newest outdoor slot */
    uint16_t out_bat_mv;              /* WS_LINK_NO_U16: unknown */
    bool     out_low_bat;
    bool     in_on;                   /* the indoor sensor switched on */
    bool     in_have;                 /* and a recent sample */
    int32_t  in_t, in_rh;
} ui_weather_now_t;

/* Any task. */
void ui_weather_now(ui_weather_now_t *n);

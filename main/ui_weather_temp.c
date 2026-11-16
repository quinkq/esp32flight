/*
 * weather-station fork: temperatures and current readings as the weather
 * station shows them.
 *
 *  - The colour scale (decided 2026-11-16, docs\mockups\temp-scale.png and
 *    temp-scale.ps1, which use the same anchors): a blend between hand-picked
 *    points, so 0 °C is whitish and 20 °C amber rather than a straight
 *    bridge from violet to red. The cold end is a medium violet and indigo,
 *    readable on the dark themes; on a light theme every colour is darkened.
 *  - The large temperature: the whole degrees in font_ws_digits_100, the
 *    decimal and the degree sign in Montserrat 44, both on one baseline, as
 *    on the screensaver mockup (docs\mockups\screensaver-2026-11-16.png). Its
 *    box hugs the digits: the labels sit higher than the box by the empty
 *    space above the digits in the font's line, and draw outside it.
 *  - The readings now: one place for the rule when outdoor values count as
 *    stale, shared by the weather view, the screensaver and the header.
 */
#include "ui_weather_temp.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "calibration.h"
#include "indoor.h"
#include "outdoor_link.h"
#include "theme.h"

/* degrees C, RGB; keep in step with docs\mockups\temp-scale.ps1. Neutral
 * white only around 3 °C, warming from there (the user, 2026-11-16: 8 °C
 * read as plain white with white at 6 °C). */
static const struct {
    int16_t t;
    uint32_t rgb;
} k_scale[] = {
    { -50, 0x8e5cf7 }, { -30, 0x6f6ff7 }, { -15, 0x4a9cff }, { -5, 0x8cc8ff },
    { 0, 0xdcefff },   { 3, 0xf0f2f0 },   { 7, 0xfbecc4 },   { 11, 0xffe09a },
    { 16, 0xffcc6a },  { 21, 0xffb347 },  { 26, 0xff9033 },  { 31, 0xff6a2a },
    { 38, 0xf23c2c },  { 50, 0xff1e1e },
};
#define N_SCALE (sizeof(k_scale) / sizeof(k_scale[0]))

#define MINUS "\xE2\x88\x92"   /* U+2212, in font_ws_digits_100 */

lv_color_t ui_temp_colour(int32_t tenths)
{
    uint32_t rgb = k_scale[N_SCALE - 1].rgb;
    if (tenths <= k_scale[0].t * 10) {
        rgb = k_scale[0].rgb;
    } else {
        for (size_t i = 1; i < N_SCALE; i++) {
            const int32_t hi = k_scale[i].t * 10;
            if (tenths <= hi) {
                const int32_t lo = k_scale[i - 1].t * 10;
                const uint32_t a = k_scale[i - 1].rgb, b = k_scale[i].rgb;
                rgb = 0;
                for (int sh = 16; sh >= 0; sh -= 8) {
                    const int32_t ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
                    rgb |= (uint32_t)(ca + (cb - ca) * (tenths - lo) / (hi - lo)) << sh;
                }
                break;
            }
        }
    }
    lv_color_t c = lv_color_hex(rgb);
    if (lv_color_brightness(app_theme()->panel) > 128) {
        c = lv_color_mix(lv_color_black(), c, 115);   /* a light theme: ~45 % towards black */
    }
    return c;
}

void ui_temp_colour_code(char *buf, size_t n, int32_t tenths)
{
    snprintf(buf, n, "#%06lx", (unsigned long)(lv_color_to32(ui_temp_colour(tenths)) & 0xffffff));
}

void ui_digits_metrics(const lv_font_t *font, lv_coord_t *top, lv_coord_t *height)
{
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(font, &g, '0', 0)) {
        *top = 0;
        *height = font->line_height;
        return;
    }
    const lv_coord_t baseline = font->line_height - font->base_line;   /* from the label's top */
    *height = g.box_h;
    *top = baseline - (g.box_h + g.ofs_y);
}

static lv_coord_t text_w(const char *text, const lv_font_t *font)
{
    lv_point_t size;
    lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

lv_obj_t *ui_temp_big_create(lv_obj_t *parent)
{
    lv_obj_t *big = lv_obj_create(parent);
    lv_obj_remove_style_all(big);
    lv_obj_add_flag(big, LV_OBJ_FLAG_OVERFLOW_VISIBLE);   /* the labels' empty line space sticks out */
    lv_obj_clear_flag(big, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *whole = lv_label_create(big);
    lv_obj_set_style_text_font(whole, &font_ws_digits_100, 0);
    lv_label_set_text(whole, "");
    lv_obj_t *rest = lv_label_create(big);
    lv_obj_set_style_text_font(rest, &lv_font_montserrat_44, 0);
    lv_label_set_text(rest, "");
    return big;
}

void ui_temp_big_set(lv_obj_t *big, bool have, int32_t tenths, lv_color_t colour, const char *suffix)
{
    lv_obj_t *whole = lv_obj_get_child(big, 0), *rest = lv_obj_get_child(big, 1);
    char w[16] = "-", r[32] = "";
    if (have) {
        const int32_t a = tenths < 0 ? -tenths : tenths;
        snprintf(w, sizeof(w), "%s%ld", tenths < 0 ? MINUS : "", (long)(a / 10));
        snprintf(r, sizeof(r), ".%ld\xC2\xB0%s", (long)(a % 10), suffix != NULL ? suffix : "");
    }
    lv_label_set_text(whole, w);
    lv_label_set_text(rest, r);
    lv_obj_set_style_text_color(whole, colour, 0);
    lv_obj_set_style_text_color(rest, colour, 0);
    /* one baseline (each font's sits line_height - base_line below its
       label's top), and the box from the digits' top to the baseline */
    const lv_font_t *fw = &font_ws_digits_100, *fr = &lv_font_montserrat_44;
    lv_coord_t top, height;
    ui_digits_metrics(fw, &top, &height);
    const lv_coord_t ww = text_w(w, fw);
    lv_obj_set_pos(whole, 0, -top);
    lv_obj_set_pos(rest, ww, -top + (fw->line_height - fw->base_line) - (fr->line_height - fr->base_line));
    lv_obj_set_size(big, ww + text_w(r, fr), height);
}

bool ui_weather_out_temp(const readings_slot_t *s, int32_t *t)
{
    if (s->flags & READINGS_SHT_OK) {
        *t = s->sht_t / 10;
        return true;
    }
    if (s->flags & READINGS_BME_OK) {   /* the backup */
        *t = s->bme_t / 10;
        return true;
    }
    return false;
}

void ui_weather_now(ui_weather_now_t *n)
{
    memset(n, 0, sizeof(*n));
    n->out_bat_mv = WS_LINK_NO_U16;
    calibration_t cal;
    calibration_get(&cal);

    readings_slot_t s;
    n->out_any = readings_store_latest(&s);
    if (n->out_any) {
        calibration_apply(&cal, &s);
        n->out_ts = s.ts;
        n->out_have_t = ui_weather_out_temp(&s, &n->out_t);
        n->out_have_rh = (s.flags & READINGS_SHT_OK) != 0;   /* the SHT40 only (open decision #6) */
        n->out_rh = s.sht_rh / 10;
        n->out_have_p = (s.flags & READINGS_BME_OK) != 0;
        n->out_p = s.bme_p;
        n->out_bat_mv = s.bat_mv;
        n->out_low_bat = (s.flags & READINGS_LOW_BATTERY) != 0;
    }
    outdoor_health_t h;
    outdoor_link_health(&h);
    const time_t now = time(NULL);
    n->out_stale = !n->out_any || h.status == OUTDOOR_SILENT ||
                   (h.status == OUTDOOR_NOT_HEARD && now - (time_t)n->out_ts > 3 * (time_t)h.expected_gap_s);

    indoor_t in;
    indoor_latest(&in);
    n->in_on = in.on;
    n->in_have = in.on && in.have && in.answering;
    if (n->in_have) {
        int16_t t = in.t;
        uint16_t rh = in.rh;
        calibration_indoor(&cal, &t, &rh);
        n->in_t = t / 10;
        n->in_rh = rh / 10;
    }
}

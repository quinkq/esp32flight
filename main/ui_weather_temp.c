/*
 * weather-station fork: temperatures as the weather station shows them.
 *
 *  - The colour scale (decided 2026-11-16, docs\mockups\temp-scale.png and
 *    temp-scale.ps1, which use the same anchors): a blend between hand-picked
 *    points, so 0 °C is whitish and 20 °C amber rather than a straight
 *    bridge from violet to red. The cold end is a medium violet and indigo,
 *    readable on the dark themes; on a light theme every colour is darkened.
 *  - The large temperature: the whole degrees in font_ws_digits_100, the
 *    decimal and the degree sign in Montserrat 44, both on one baseline, as
 *    on the screensaver mockup (docs\mockups\screensaver-2026-11-16.png).
 */
#include "ui_weather_temp.h"

#include <stdio.h>
#include "theme.h"

/* degrees C, RGB; keep in step with docs\mockups\temp-scale.ps1 */
static const struct {
    int16_t t;
    uint32_t rgb;
} k_scale[] = {
    { -50, 0x8e5cf7 }, { -30, 0x6f6ff7 }, { -15, 0x4a9cff }, { -5, 0x8cc8ff },
    { 0, 0xdcefff },   { 6, 0xf1f1e8 },   { 12, 0xffe6a6 },  { 17, 0xffcf70 },
    { 21, 0xffb347 },  { 26, 0xff9033 },  { 31, 0xff6a2a },  { 38, 0xf23c2c },
    { 50, 0xff1e1e },
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

lv_obj_t *ui_temp_big_create(lv_obj_t *parent)
{
    lv_obj_t *big = lv_obj_create(parent);
    lv_obj_remove_style_all(big);
    lv_obj_set_size(big, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(big, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *whole = lv_label_create(big);
    lv_obj_set_style_text_font(whole, &font_ws_digits_100, 0);
    lv_label_set_text(whole, "");
    lv_obj_t *rest = lv_label_create(big);
    lv_obj_set_style_text_font(rest, &lv_font_montserrat_44, 0);
    lv_label_set_text(rest, "");
    return big;
}

static lv_coord_t text_w(const char *text, const lv_font_t *font)
{
    lv_point_t size;
    lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
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
    /* one baseline: each font's baseline sits line_height - base_line from its top */
    const lv_font_t *fw = &font_ws_digits_100, *fr = &lv_font_montserrat_44;
    lv_obj_set_pos(whole, 0, 0);
    lv_obj_set_pos(rest, text_w(w, fw), (fw->line_height - fw->base_line) - (fr->line_height - fr->base_line));
}

/*
 * weather-station fork: the weather station on the screensaver and in the
 * header (designed 2026-11-16, DISPLAY.md "UI"; mockup
 * docs\mockups\screensaver-2026-11-16.png, made with screensaver-mockup.ps1).
 *
 * The screensaver (map style; upstream builds it, ui_weather_amb_build()
 * adds ours on top, in boxes like upstream's labels):
 *  - top left: the clock in 94 px digits (font_ws_digits_94; "AM"/"PM" from
 *    Montserrat 44 as its fallback), the date under it. Upstream's clock
 *    label stays, hidden: ours copies its text, so the time source is the
 *    same. 94 px reaches the ring at ~9 % of the day's minutes (wide times
 *    like "00:00"; the user's choice, 2026-11-18).
 *  - bottom left, outdoor; bottom right, indoor: the large temperature in
 *    110 px digits (ui_weather_temp.c, the colour scale) and under it the
 *    place's icon, name and humidity. Outdoor grey with "no data" when
 *    stale; indoor hidden while its sensor is switched off. The outdoor one
 *    reaches the ring from about -10 °C (accepted).
 *  - top right: Open-Meteo without its temperature (our sensors measure
 *    that): its icon and words, the wind under them. Upstream's label for
 *    it stays, hidden.
 *  - the map credit moves to the bottom centre.
 *  - taps (the user, 2026-11-18): on a weather field or anywhere outside the
 *    radar's ring, the weather view; inside the ring as upstream has it: a
 *    plane is picked, empty map closes the screensaver onto the page that
 *    was open (the weather view too). The check sits in upstream's tap
 *    handler after its plane handling; ours aren't clickable, so its first
 *    tap still only wakes a dark screen. The ring's place comes from
 *    upstream's ring object.
 *  Sizes as measured on the mockups (docs\mockups\): the ring spans the
 *  screen's height (centre 512,300, radius ~306).
 *
 * The header: our outdoor / indoor temperatures in the scale's colours, with
 * Open-Meteo's icon, wind and words; Open-Meteo's own temperature, marked
 * "~", only while the outdoor unit is silent. Kept up to date every minute
 * and on every Open-Meteo answer (the hook in flight_task.c). Tapping it
 * opens the weather view. Fahrenheit (upstream's setting) keeps upstream's
 * line: ours are in °C.
 */
#include "ui_weather_amb.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "lang.h"
#include "settings.h"
#include "ui.h"
#include "ui_scale.h"
#include "ui_weather_temp.h"
#include "ui_weather_view.h"

#define MARGIN_X   UISX(10)
#define MARGIN_Y   UISY(8)
#define PAD        UISX(8)       /* in a box, around large digits */
#define PAD_LINE   UISY(4)       /* a text line's box, above and below */
#define GAP        UISY(4)       /* between a box and the next */
#define TICK_MS    1000          /* the clock follows within a second */
#define DATA_TICKS 30            /* readings every 30 s */
#define HEADER_MS  60000

#define COL_WHITE  lv_color_white()
#define COL_GREY   lv_color_hex(0xc8cdd2)
#define COL_STALE  lv_color_hex(0x8a8f94)

#define ICON_TREE  "\xEF\x86\xBB"   /* f1bb */
#define ICON_WIND  "\xEF\x9C\xAE"   /* f72e */

typedef struct {
    const char *place[2], *no_data, *wind;   /* wind: %s direction, %.0f km/h */
    const char *weekday[7], *month[12];
} strings_t;

static const strings_t k_en = {
    .place = { "Outdoor", "Indoor" },
    .no_data = "no data",
    .wind = "wind %s %.0f km/h",
    .weekday = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" },
    .month = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" },
};

static const strings_t k_pl = {
    .place = { "Na zewnątrz", "W domu" },
    .no_data = "brak danych",
    .wind = "wiatr %s %.0f km/h",
    .weekday = { "niedz.", "pon.", "wt.", "śr.", "czw.", "pt.", "sob." },
    .month = { "sty", "lut", "mar", "kwi", "maj", "cze", "lip", "sie", "wrz", "paź", "lis", "gru" },
};

static const strings_t *S(void)
{
    return settings_get()->lang == 1 ? &k_pl : &k_en;
}

/* the screensaver's objects; all NULL while it's closed */
static struct {
    lv_obj_t *up_clock;                    /* upstream's, hidden: the time we show */
    lv_obj_t *ring;                        /* upstream's radar ring: inside it, taps stay upstream's */
    lv_obj_t *clock_box, *clock, *date;
    lv_obj_t *num_box[2], *big[2], *line[2];   /* outdoor, indoor */
    lv_obj_t *wx[2];                       /* Open-Meteo: words, wind */
    lv_timer_t *timer;
    int ticks;
} s_a;

static lv_font_t s_clock_font;             /* font_ws_digits_94 with a fallback for "AM"/"PM" */

static struct {
    lv_obj_t *label;
    lv_timer_t *timer;
} s_hdr;

/* Open-Meteo's latest answer, written by the flight task */
static portMUX_TYPE s_wx_mux = portMUX_INITIALIZER_UNLOCKED;
static weather_t s_wx;
static bool s_wx_have;

static bool wx_get(weather_t *out)
{
    taskENTER_CRITICAL(&s_wx_mux);
    *out = s_wx;
    const bool have = s_wx_have;
    taskEXIT_CRITICAL(&s_wx_mux);
    return have;
}

static void tenths(char *buf, size_t n, int32_t v)
{
    const int32_t a = v < 0 ? -v : v;
    snprintf(buf, n, "%s%ld.%ld", v < 0 ? "-" : "", (long)(a / 10), (long)(a % 10));
}

static lv_coord_t text_w(const char *text, const lv_font_t *font)
{
    lv_point_t size;
    lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* a box like upstream's screensaver labels: black at 40 % */
static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

/* a text line in such a box (the label is its own box) */
static lv_obj_t *line_box(lv_obj_t *parent, lv_color_t colour)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &font_ws_26, 0);
    lv_obj_set_style_text_color(l, colour, 0);
    lv_obj_set_style_bg_color(l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_40, 0);
    lv_obj_set_style_pad_hor(l, PAD, 0);
    lv_obj_set_style_pad_ver(l, PAD_LINE, 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_coord_t line_h(void)
{
    return font_ws_26.line_height + 2 * PAD_LINE;
}

/* a line's box at x (or ending at x when right), y */
static void place_line(lv_obj_t *l, const char *text, lv_coord_t x, lv_coord_t y, bool right)
{
    lv_label_set_text(l, text);
    const lv_coord_t w = text_w(text, &font_ws_26) + 2 * PAD;
    lv_obj_set_pos(l, right ? x - w : x, y);
}

/* ---- the screensaver ---- */

static void clock_update(void)
{
    const char *t = lv_label_get_text(s_a.up_clock);
    if (strcmp(t, lv_label_get_text(s_a.clock)) == 0) {
        return;
    }
    lv_label_set_text(s_a.clock, t);
    lv_coord_t top, height;
    ui_digits_metrics(&font_ws_digits_94, &top, &height);
    lv_obj_set_pos(s_a.clock, PAD, PAD - top);
    lv_obj_set_size(s_a.clock_box, text_w(t, &s_clock_font) + 2 * PAD, height + 2 * PAD);

    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char d[32];
    snprintf(d, sizeof(d), "%s %d %s", S()->weekday[tm.tm_wday], tm.tm_mday, S()->month[tm.tm_mon]);
    place_line(s_a.date, d, MARGIN_X, MARGIN_Y + height + 2 * PAD + GAP, false);
}

/* outdoor (k 0) or indoor (k 1): the number box above the line box, in its corner */
static void readout(int k, bool show, bool have, int32_t t, bool stale, const char *line)
{
    if (!show) {
        lv_obj_add_flag(s_a.num_box[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_a.line[k], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_a.num_box[k], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_a.line[k], LV_OBJ_FLAG_HIDDEN);
    const bool right = k == 1;
    const lv_coord_t x = right ? LV_HOR_RES - MARGIN_X : MARGIN_X;
    const lv_coord_t ly = LV_VER_RES - MARGIN_Y - line_h();
    lv_obj_set_style_text_color(s_a.line[k], stale ? COL_STALE : COL_GREY, 0);
    place_line(s_a.line[k], line, x, ly, right);

    ui_temp_big_set(s_a.big[k], have, t, stale || !have ? COL_STALE : ui_temp_colour(t), NULL);
    /* its size, as ui_temp_big_set() set it from the text */
    const lv_coord_t w = lv_obj_get_style_width(s_a.big[k], 0) + 2 * PAD;
    const lv_coord_t h = lv_obj_get_style_height(s_a.big[k], 0) + 2 * PAD;
    lv_obj_set_size(s_a.num_box[k], w, h);
    lv_obj_set_pos(s_a.big[k], PAD, PAD);
    lv_obj_set_pos(s_a.num_box[k], right ? x - w : x, ly - GAP - h);
}

static void data_update(void)
{
    const strings_t *str = S();
    ui_weather_now_t n;
    ui_weather_now(&n);
    char line[96];

    /* outdoor */
    if (n.out_stale || !n.out_have_rh) {
        snprintf(line, sizeof(line), "%s  %s \xC2\xB7 %s", ICON_TREE, str->place[0],
                 n.out_stale ? str->no_data : "-");
    } else {
        snprintf(line, sizeof(line), "%s  %s \xC2\xB7 %ld %%", ICON_TREE, str->place[0], (long)((n.out_rh + 5) / 10));
    }
    readout(0, true, n.out_have_t, n.out_t, n.out_stale, line);

    /* indoor: hidden while its sensor is off */
    if (n.in_have) {
        snprintf(line, sizeof(line), "%s  %s \xC2\xB7 %ld %%", LV_SYMBOL_HOME, str->place[1], (long)((n.in_rh + 5) / 10));
    } else {
        snprintf(line, sizeof(line), "%s  %s \xC2\xB7 %s", LV_SYMBOL_HOME, str->place[1], str->no_data);
    }
    readout(1, n.in_on, n.in_have, n.in_t, !n.in_have, line);

    /* Open-Meteo, without its temperature */
    weather_t wx;
    if (!wx_get(&wx)) {
        lv_obj_add_flag(s_a.wx[0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_a.wx[1], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    snprintf(line, sizeof(line), "%s  %s", weather_icon_str(wx.code), lang_weather_desc(wx.code));
    place_line(s_a.wx[0], line, LV_HOR_RES - MARGIN_X, MARGIN_Y, true);
    snprintf(line, sizeof(line), str->wind, wx.wind_dir_deg >= 0 ? lang_compass(wx.wind_dir_deg) : "",
             (double)wx.wind_kmh);
    place_line(s_a.wx[1], line, LV_HOR_RES - MARGIN_X, MARGIN_Y + line_h() + GAP, true);
    lv_obj_clear_flag(s_a.wx[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_a.wx[1], LV_OBJ_FLAG_HIDDEN);
}

static void amb_timer_cb(lv_timer_t *t)
{
    clock_update();
    if (++s_a.ticks >= DATA_TICKS) {
        s_a.ticks = 0;
        data_update();
    }
}

/* the screensaver is gone, ours with it */
static void amb_deleted_cb(lv_event_t *e)
{
    if (s_a.timer != NULL) {
        lv_timer_del(s_a.timer);
    }
    memset(&s_a, 0, sizeof(s_a));
}

void ui_weather_amb_build(lv_obj_t *amb, lv_obj_t *clock, lv_obj_t *wx, lv_obj_t *attr, lv_obj_t *ring)
{
    memset(&s_a, 0, sizeof(s_a));
    s_a.up_clock = clock;
    s_a.ring = ring;
    lv_obj_add_flag(clock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wx, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(attr, LV_ALIGN_BOTTOM_MID, 0, -UISY(4));   /* the corners are ours now */

    s_clock_font = font_ws_digits_94;
    s_clock_font.fallback = &lv_font_montserrat_44;
    s_a.clock_box = box(amb);
    lv_obj_set_pos(s_a.clock_box, MARGIN_X, MARGIN_Y);
    s_a.clock = lv_label_create(s_a.clock_box);
    lv_obj_set_style_text_font(s_a.clock, &s_clock_font, 0);
    lv_obj_set_style_text_color(s_a.clock, COL_WHITE, 0);
    lv_label_set_text(s_a.clock, "");
    s_a.date = line_box(amb, COL_GREY);

    for (int k = 0; k < 2; k++) {
        s_a.num_box[k] = box(amb);
        s_a.big[k] = ui_temp_big_create(s_a.num_box[k], &font_ws_digits_110);
        s_a.line[k] = line_box(amb, COL_GREY);
        s_a.wx[k] = line_box(amb, k == 0 ? COL_WHITE : COL_GREY);
    }
    lv_obj_add_event_cb(s_a.clock_box, amb_deleted_cb, LV_EVENT_DELETE, NULL);

    clock_update();
    data_update();
    s_a.timer = lv_timer_create(amb_timer_cb, TICK_MS, NULL);
}

static bool hit(lv_obj_t *o, const lv_point_t *pt)
{
    if (o == NULL || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return _lv_area_is_point_on(&a, pt, 0);
}

/* outside the radar's ring; false while upstream hides it (no map yet) */
static bool outside_ring(const lv_point_t *pt)
{
    if (s_a.ring == NULL || lv_obj_has_flag(s_a.ring, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    lv_area_t a;
    lv_obj_get_coords(s_a.ring, &a);
    const int32_t r = lv_area_get_width(&a) / 2;
    const int32_t dx = pt->x - (a.x1 + r), dy = pt->y - (a.y1 + r);
    return dx * dx + dy * dy > r * r;
}

bool ui_weather_amb_hit(const lv_point_t *pt)
{
    if (s_a.up_clock == NULL) {
        return false;   /* not our screensaver (the retro style) */
    }
    for (int k = 0; k < 2; k++) {
        if (hit(s_a.num_box[k], pt) || hit(s_a.line[k], pt) || hit(s_a.wx[k], pt)) {
            return true;
        }
    }
    return outside_ring(pt);
}

/* ---- the header ---- */

/* our header line into buf; false: nothing of ours to show (upstream's line stays) */
static bool header_compose(char *buf, size_t n)
{
    if (settings_get()->temp_f) {
        return false;   /* ours are °C */
    }
    ui_weather_now_t now;
    ui_weather_now(&now);
    weather_t wx;
    const bool have_wx = wx_get(&wx);
    const bool out_ok = now.out_have_t && !now.out_stale;
    if (!out_ok && !now.in_have) {
        return false;
    }
    char out[40], in[40] = "", code[8], a[16];
    if (out_ok) {
        tenths(a, sizeof(a), now.out_t);
        ui_temp_colour_code(code, sizeof(code), now.out_t);
        snprintf(out, sizeof(out), "%s %s\xC2\xB0#", code, a);
    } else if (have_wx) {
        snprintf(out, sizeof(out), "~%.0f\xC2\xB0", (double)wx.temp_c);   /* Open-Meteo's: a model value */
    } else {
        snprintf(out, sizeof(out), "-");
    }
    if (now.in_have) {
        tenths(a, sizeof(a), now.in_t);
        ui_temp_colour_code(code, sizeof(code), now.in_t);
        snprintf(in, sizeof(in), " / %s %s\xC2\xB0#", code, a);
    }
    if (have_wx) {
        /* wind before the words, as upstream: truncation trims words, not data */
        snprintf(buf, n, "%s %s%s  " ICON_WIND " %s %.0f km/h  %s", weather_icon_str(wx.code), out, in,
                 wx.wind_dir_deg >= 0 ? lang_compass(wx.wind_dir_deg) : "", (double)wx.wind_kmh,
                 lang_weather_desc(wx.code));
    } else {
        snprintf(buf, n, "%s%s", out, in);
    }
    return true;
}

static void header_timer_cb(lv_timer_t *t)
{
    char buf[96];
    if (header_compose(buf, sizeof(buf))) {
        ui_set_weather(buf);
    }
}

static void header_click_cb(lv_event_t *e)
{
    ui_show_weather_view();
}

void ui_weather_header_hook(lv_obj_t *label)
{
    s_hdr.label = label;
    lv_label_set_recolor(label, true);   /* the temperatures in their colours */
    lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(label, UISY(10));
    lv_obj_add_event_cb(label, header_click_cb, LV_EVENT_CLICKED, NULL);
    s_hdr.timer = lv_timer_create(header_timer_cb, HEADER_MS, NULL);
}

void ui_weather_open_meteo(const weather_t *wx, char *text, size_t n)
{
    taskENTER_CRITICAL(&s_wx_mux);
    s_wx = *wx;
    s_wx_have = wx->valid;
    taskEXIT_CRITICAL(&s_wx_mux);
    header_compose(text, n);   /* leaves upstream's line when we have nothing */
}

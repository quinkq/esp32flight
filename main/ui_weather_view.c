/*
 * weather-station fork: the weather view, the sixth view (designed
 * 2026-11-16, DISPLAY.md "UI"). It covers everything below the header, the
 * aircraft list too (the user, after the first try).
 *
 *  - Top: outdoor from the left, indoor from the right: the temperature in
 *    the screensaver's 100 px digits (outdoor with its 3-hour trend), beside
 *    it humidity (and outdoor pressure with its trend), under it the
 *    temperature's minimum and maximum over the chart's window (24 hours or
 *    7 days, from the stored slots). The outdoor unit's status beside its
 *    caption. Temperatures in the colour scale of ui_weather_temp.c, the
 *    minimum and maximum each in its own. Outdoor values go grey when it
 *    counts as silent (or, after a display restart, when the newest reading
 *    is older than three expected gaps).
 *  - One chart: [1 day | 7 days] (the 96 stored 15-minute slots; 168 hourly
 *    means), toggles for outdoor and indoor (either or both) and for
 *    temperature, humidity and pressure (any combination, at least one).
 *    The first quantity switched on (in that order) gets the left axis, the
 *    second the right; with all three, pressure is drawn on the right axis's
 *    scale and its lowest and highest values are written next to the line
 *    instead. Outdoor lines thick in the quantity's colour, indoor lines
 *    thin in a pale shade of it (the draw event sets the width per series;
 *    LVGL 8 can't dash slanted lines); there's no indoor pressure.
 *  - Values calibrated as everywhere (calibration.c). Outdoor temperature
 *    from the SHT40, the BME280's as a backup; humidity from the SHT40 only
 *    (open decision #6); pressure from the BME280.
 *  - Built once with the other panels (ui.c), refreshed every minute while
 *    shown and on every tap; everything runs in the LVGL task.
 */
#include "ui_weather_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "calibration.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "fonts.h"
#include "indoor.h"
#include "outdoor_link.h"
#include "readings_store.h"
#include "settings.h"
#include "theme.h"
#include "ui_scale.h"
#include "ui_weather_temp.h"

#define COL_PANEL  (app_theme()->panel)
#define COL_ROW    (app_theme()->row)
#define COL_ACCENT (app_theme()->accent)
#define COL_TEXT   (app_theme()->text)
#define COL_DIM    (app_theme()->dim)
#define COL_BAD    lv_color_hex(0xff6b6b)

#define REFRESH_MS     60000
#define CLOCK_VALID_FROM 1767225600      /* 2026-01-01: earlier means no time yet */
#define DAY_POINTS     96                /* 15-minute slots */
#define WEEK_POINTS    168               /* hourly means */
#define MAX_POINTS     WEEK_POINTS
#define TREND_S        (3 * 3600)
#define TREND_SLOTS    20                /* 4 h of slots and some */
#define TREND_T        5                 /* 0.5 °C in 3 h: an arrow */
#define TREND_P        10                /* 1.0 hPa in 3 h */
#define TREND_H        50                /* 5 % in 3 h: past the SHT40's jitter */
#define AXIS_LABEL_W   56                /* room for the axis labels left and right of the chart */
#define LINE_OUT       4                 /* line widths */
#define LINE_IN        2
#define MAX_XLABELS    10
#define MAX_SEPS       8

/* quantities; values in tenths: 0.1 °C, 0.1 %, 0.1 hPa */
enum { Q_T, Q_H, Q_P, N_Q };
enum { OUT, IN, N_WHERE };

/* readable on the dark and the light themes */
static const uint32_t k_colour[N_Q] = { 0xe8743b, 0x2f9be0, 0x4caf50 };

typedef struct {
    const char *outdoor, *indoor, *min, *max, *quantity[N_Q], *day, *week, *where[N_WHERE];
    const char *no_data, *no_data_since, *off, *no_time;
    const char *link_off, *not_heard, *heard, *silent, *battery, *low;   /* the outdoor unit, short: beside its caption */
    const char *dur_s, *dur_min, *dur_h;
    const char *weekday[7];              /* Sunday first, as struct tm */
} strings_t;

static const strings_t k_en = {
    .outdoor = "Outdoor", .indoor = "Indoor", .min = "min", .max = "max",
    .quantity = { "Temperature", "Humidity", "Pressure" },
    .day = "1 day", .week = "7 days", .where = { "Outdoor", "Indoor" },
    .no_data = "no data", .no_data_since = "no data since %s", .off = "off",
    .no_time = "Waiting for the time...",
    .link_off = LV_SYMBOL_WIFI "  link off",
    .not_heard = LV_SYMBOL_WIFI "  not heard yet",
    .heard = LV_SYMBOL_WIFI "  %s ago",
    .silent = LV_SYMBOL_WIFI "  silent for %s",
    .battery = " · battery %.2f V", .low = " (low)",
    .dur_s = "%d s", .dur_min = "%d min", .dur_h = "%d h %d min",
    .weekday = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" },
};

static const strings_t k_pl = {
    .outdoor = "Na zewnątrz", .indoor = "W domu", .min = "min", .max = "maks",
    .quantity = { "Temperatura", "Wilgotność", "Ciśnienie" },
    .day = "1 dzień", .week = "7 dni", .where = { "Na zewnątrz", "W domu" },
    .no_data = "brak danych", .no_data_since = "brak danych od %s", .off = "wyłączony",
    .no_time = "Czekam na czas...",
    .link_off = LV_SYMBOL_WIFI "  połączenie wyłączone",
    .not_heard = LV_SYMBOL_WIFI "  jeszcze nie odebrano",
    .heard = LV_SYMBOL_WIFI "  %s temu",
    .silent = LV_SYMBOL_WIFI "  milczy od %s",
    .battery = " · bateria %.2f V", .low = " (niska)",
    .dur_s = "%d s", .dur_min = "%d min", .dur_h = "%d godz. %d min",
    .weekday = { "niedz.", "pon.", "wt.", "śr.", "czw.", "pt.", "sob." },
};

static const strings_t *S(void)
{
    return settings_get()->lang == 1 ? &k_pl : &k_en;
}

/* The places, in OUT, IN order: an icon each (font_ws_26), the names in
 * strings_t.where. A table, so extra sensors become rows: the plan
 * (2026-11-16, DISPLAY.md "UI") is outdoor fixed on the left and the right
 * card's caption a menu choosing indoor or a sensor, the chart showing
 * outdoor and the chosen one. */
#define ICON_TREE "\xEF\x86\xBB"   /* f1bb */
static const char *const k_place_icon[N_WHERE] = { ICON_TREE, LV_SYMBOL_HOME };

/* the chart's sums per point, PSRAM */
typedef struct {
    int32_t sum[N_WHERE][N_Q][MAX_POINTS];
    uint16_t n[N_WHERE][N_Q][MAX_POINTS];
} acc_t;

static struct {
    lv_obj_t *panel;
    bool shown;
    lv_timer_t *timer;
    /* the top: captions, the large temperatures (ui_weather_temp.c), the
       details beside them, the minimum and maximum under them */
    lv_obj_t *out_cap, *out_big, *out_det[2], *out_mm, *in_cap, *in_big, *in_det, *in_mm, *status;
    /* the temperature's minimum and maximum over the chart's window, from the
       stored slots (not the hourly means, which would flatten them) */
    bool mm_have[N_WHERE];
    int32_t mm_min[N_WHERE], mm_max[N_WHERE];
    /* the chart */
    lv_obj_t *range_btn[2], *where_btn[N_WHERE], *q_btn[N_Q];
    lv_obj_t *chart;
    lv_chart_series_t *ser[N_WHERE][N_Q];  /* [IN][Q_P] stays NULL */
    lv_obj_t *xlab[MAX_XLABELS], *sep[MAX_SEPS], *p_lab[2];
    lv_coord_t cx, cy, cw, ch;             /* the chart's place in the panel */
    acc_t *acc;
    /* what's drawn */
    bool week;
    uint8_t on;                            /* bit per quantity */
    uint8_t where_on;                      /* bit per OUT, IN */
    int8_t axis_q[2];                      /* the quantity on the left and right axis; -1: none */
    int32_t axis_step[2];
} s_v = { .on = 1u << Q_T, .where_on = (1u << OUT) | (1u << IN), .axis_q = { Q_T, -1 } };

/* ---- helpers ---- */

static lv_color_t q_colour(int q, int where)
{
    const lv_color_t c = lv_color_hex(k_colour[q]);
    /* indoor: halfway to the text colour, so pastel on the dark themes and
       darker on the light ones; with the thin line it stands apart */
    return where == OUT ? c : lv_color_mix(c, COL_TEXT, 128);
}

static lv_coord_t text_w(const char *text, const lv_font_t *font)
{
    lv_point_t size;
    lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t colour)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, colour, 0);
    lv_label_set_text(l, "");
    return l;
}

static void dur(char *buf, size_t n, int64_t s)
{
    if (s < 90) {
        snprintf(buf, n, S()->dur_s, (int)s);
    } else if (s < 90 * 60) {
        snprintf(buf, n, S()->dur_min, (int)((s + 30) / 60));
    } else {
        const int m = (int)((s + 30) / 60);
        snprintf(buf, n, S()->dur_h, m / 60, m % 60);
    }
}

/* a tenths value as "-3.4" */
static void tenths(char *buf, size_t n, int32_t v)
{
    snprintf(buf, n, "%s%ld.%ld", v < 0 ? "-" : "", (long)(v < 0 ? -v : v) / 10, (long)(v < 0 ? -v : v) % 10);
}

static int32_t floor_div(int32_t a, int32_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

#define out_temp ui_weather_out_temp   /* the SHT40, the BME280 as the backup (ui_weather_temp.c) */

static void hm(char *buf, size_t n, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

/* ---- the top: outdoor and indoor now ---- */

typedef struct {
    calibration_t cal;
    int n;                                 /* the trend's slots */
    struct {
        uint32_t ts;
        int32_t v[N_Q];                    /* outdoor temperature, humidity, pressure, tenths */
        bool have[N_Q];
    } slot[TREND_SLOTS];
} top_t;

static bool trend_cb(const readings_slot_t *stored, void *ctx)
{
    top_t *g = ctx;
    if (g->n >= TREND_SLOTS) {
        return false;
    }
    readings_slot_t s = *stored;
    calibration_apply(&g->cal, &s);
    g->slot[g->n].ts = s.ts;
    g->slot[g->n].have[Q_T] = out_temp(&s, &g->slot[g->n].v[Q_T]);
    g->slot[g->n].have[Q_H] = (s.flags & READINGS_SHT_OK) != 0;
    g->slot[g->n].v[Q_H] = s.sht_rh / 10;
    g->slot[g->n].have[Q_P] = (s.flags & READINGS_BME_OK) != 0;
    g->slot[g->n].v[Q_P] = s.bme_p;
    g->n++;
    return true;
}

/* change over the last 3 hours: +1, -1, 0 (steady, or not known) */
static int trend(const top_t *g, int q)
{
    static const int32_t limit[N_Q] = { TREND_T, TREND_H, TREND_P };
    int newest = -1;
    for (int i = 0; i < g->n; i++) {
        if (g->slot[i].have[q] && (newest < 0 || g->slot[i].ts > g->slot[newest].ts)) {
            newest = i;
        }
    }
    if (newest < 0) {
        return 0;
    }
    const uint32_t want = g->slot[newest].ts - TREND_S;
    int old = -1;
    uint32_t best = 1800;   /* within half an hour of 3 h before */
    for (int i = 0; i < g->n; i++) {
        const uint32_t off = g->slot[i].ts > want ? g->slot[i].ts - want : want - g->slot[i].ts;
        if (g->slot[i].have[q] && off <= best) {
            best = off;
            old = i;
        }
    }
    if (old < 0) {
        return 0;
    }
    const int32_t d = g->slot[newest].v[q] - g->slot[old].v[q];
    return d >= limit[q] ? 1 : d <= -limit[q] ? -1 : 0;
}

static const char *arrow(int t)
{
    return t > 0 ? " " LV_SYMBOL_UP : t < 0 ? " " LV_SYMBOL_DOWN : "";
}

/* "min -5.1°  ·  max -1.2°" over the chart's window (its range buttons say
 * which), each value in its own colour (label recolouring: "#rrggbb
 * text#"); empty without data in the window */
static void minmax_line(lv_obj_t *l, int where)
{
    const strings_t *str = S();
    char a[16], b[16], line[128] = "";
    if (s_v.mm_have[where]) {
        tenths(a, sizeof(a), s_v.mm_min[where]);
        tenths(b, sizeof(b), s_v.mm_max[where]);
        snprintf(line, sizeof(line), "%s #%06lx %s\xC2\xB0#  \xC2\xB7  %s #%06lx %s\xC2\xB0#", str->min,
                 (unsigned long)(lv_color_to32(ui_temp_colour(s_v.mm_min[where])) & 0xffffff), a, str->max,
                 (unsigned long)(lv_color_to32(ui_temp_colour(s_v.mm_max[where])) & 0xffffff), b);
    }
    lv_obj_set_style_text_color(l, COL_DIM, 0);
    lv_label_set_text(l, line);
}

static void render_top(time_t now, const calibration_t *cal)
{
    const strings_t *str = S();
    char a[24], line[128];

    readings_slot_t latest;
    const bool have = readings_store_latest(&latest);
    if (have) {
        calibration_apply(cal, &latest);
    }
    outdoor_health_t h;
    outdoor_link_health(&h);
    ui_weather_now_t nw;
    ui_weather_now(&nw);   /* the one rule for stale outdoor values */
    const bool stale = nw.out_stale;

    top_t *g = heap_caps_calloc(1, sizeof(top_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (g == NULL) {
        return;
    }
    g->cal = *cal;
    readings_store_changed(NULL, (uint32_t)now - TREND_S - 3600, trend_cb, g);

    /* outdoor: the large temperature in the scale's colour (grey when stale) */
    int32_t t;
    const bool have_t = have && out_temp(&latest, &t);
    ui_temp_big_set(s_v.out_big, have_t, have_t ? t : 0, stale || !have_t ? COL_DIM : ui_temp_colour(t),
                    stale ? NULL : arrow(trend(g, Q_T)));

    /* beside it: humidity, then pressure, each with its trend */
    line[0] = '\0';
    if (have && (latest.flags & READINGS_SHT_OK)) {
        snprintf(line, sizeof(line), "%d %%%s", (latest.sht_rh + 50) / 100, stale ? "" : arrow(trend(g, Q_H)));
    }
    lv_label_set_text(s_v.out_det[0], line);
    line[0] = '\0';
    if (have && (latest.flags & READINGS_BME_OK)) {
        tenths(a, sizeof(a), latest.bme_p);
        snprintf(line, sizeof(line), "%s hPa%s", a, stale ? "" : arrow(trend(g, Q_P)));
    }
    lv_label_set_text(s_v.out_det[1], line);
    for (int k = 0; k < 2; k++) {
        lv_obj_set_style_text_color(s_v.out_det[k], stale ? COL_DIM : COL_TEXT, 0);
    }
    free(g);

    /* under it: the minimum and maximum over the chart's window, or since
       when there's no data */
    if (stale) {
        if (have) {
            hm(a, sizeof(a), latest.ts);
            snprintf(line, sizeof(line), str->no_data_since, a);
        } else {
            snprintf(line, sizeof(line), "%s", str->no_data);
        }
        lv_obj_set_style_text_color(s_v.out_mm, COL_BAD, 0);
        lv_label_set_text(s_v.out_mm, line);
    } else {
        minmax_line(s_v.out_mm, OUT);
    }

    /* indoor */
    indoor_t in;
    indoor_latest(&in);
    lv_obj_set_style_text_color(s_v.in_det, COL_TEXT, 0);
    if (!in.on || !in.have || !in.answering) {
        ui_temp_big_set(s_v.in_big, false, 0, COL_DIM, NULL);
        lv_obj_set_style_text_color(s_v.in_det, COL_DIM, 0);
        lv_label_set_text(s_v.in_det, in.on ? str->no_data : str->off);
    } else {
        int16_t it = in.t;
        uint16_t irh = in.rh;
        calibration_indoor(cal, &it, &irh);
        ui_temp_big_set(s_v.in_big, true, it / 10, ui_temp_colour(it / 10), NULL);
        snprintf(line, sizeof(line), "%d %%", (irh + 50) / 100);
        lv_label_set_text(s_v.in_det, line);
    }
    minmax_line(s_v.in_mm, IN);

    /* the outdoor unit */
    const int64_t ago = h.last_heard_us != 0 ? (esp_timer_get_time() - h.last_heard_us) / 1000000 : -1;
    lv_obj_set_style_text_color(s_v.status, COL_DIM, 0);
    if (!h.running) {
        snprintf(line, sizeof(line), "%s", str->link_off);
    } else if (ago < 0) {
        snprintf(line, sizeof(line), "%s", str->not_heard);
    } else {
        dur(a, sizeof(a), ago);
        snprintf(line, sizeof(line), h.status == OUTDOOR_SILENT ? str->silent : str->heard, a);
        if (h.status == OUTDOOR_SILENT) {
            lv_obj_set_style_text_color(s_v.status, COL_BAD, 0);
        }
    }
    /* the battery from the newest stored reading; not before the first frame
       since the display started: it would be from before the restart */
    if (have && h.running && ago >= 0 && latest.bat_mv != WS_LINK_NO_U16) {
        snprintf(line + strlen(line), sizeof(line) - strlen(line), str->battery, latest.bat_mv / 1000.0);
        if (latest.flags & READINGS_LOW_BATTERY) {
            snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s", str->low);
        }
    }
    lv_label_set_text(s_v.status, line);
}

/* ---- the chart ---- */

typedef struct {
    calibration_t cal;
    uint32_t base, step;
    int points;
    acc_t *acc;
} gather_t;

static void add(acc_t *acc, int where, int q, int i, int32_t v)
{
    acc->sum[where][q][i] += v;
    acc->n[where][q][i]++;
}

static void minmax(int where, int32_t t)
{
    if (!s_v.mm_have[where] || t < s_v.mm_min[where]) {
        s_v.mm_min[where] = t;
    }
    if (!s_v.mm_have[where] || t > s_v.mm_max[where]) {
        s_v.mm_max[where] = t;
    }
    s_v.mm_have[where] = true;
}

static bool gather_cb(const readings_slot_t *stored, void *ctx)
{
    gather_t *g = ctx;
    if (stored->ts < g->base) {
        return true;
    }
    const uint32_t i = (stored->ts - g->base) / g->step;
    if (i >= (uint32_t)g->points) {
        return true;
    }
    readings_slot_t s = *stored;
    calibration_apply(&g->cal, &s);
    int32_t t;
    if (out_temp(&s, &t)) {
        add(g->acc, OUT, Q_T, i, t);
        minmax(OUT, t);
    }
    if (s.flags & READINGS_SHT_OK) {
        add(g->acc, OUT, Q_H, i, s.sht_rh / 10);
    }
    if (s.flags & READINGS_BME_OK) {
        add(g->acc, OUT, Q_P, i, s.bme_p);
    }
    if (s.flags & READINGS_INDOOR_OK) {
        add(g->acc, IN, Q_T, i, s.in_t / 10);
        add(g->acc, IN, Q_H, i, s.in_rh / 10);
        minmax(IN, s.in_t / 10);
    }
    return true;
}

/* a range of 4 ticks with round steps around lo..hi; returns the step */
static int32_t nice_range(int q, bool any, int32_t lo, int32_t hi, int32_t *a, int32_t *b)
{
    static const int32_t steps[N_Q][6] = {
        { 5, 10, 20, 50, 100, 200 },       /* 0.5 .. 20 °C */
        { 50, 100, 200, 250, 250, 250 },   /* 5 .. 25 % */
        { 5, 10, 20, 50, 100, 200 },       /* 0.5 .. 20 hPa */
    };
    static const int32_t dflt[N_Q][2] = { { 0, 200 }, { 0, 1000 }, { 9900, 10300 } };
    if (!any) {
        lo = dflt[q][0];
        hi = dflt[q][1];
    }
    int32_t step = steps[q][5];
    for (int k = 0; k < 6; k++) {
        step = steps[q][k];
        *a = floor_div(lo, step) * step;
        *b = *a + 4 * step;
        if (*b >= hi) {
            break;
        }
    }
    if (q == Q_H && *b > 1000) {           /* humidity stays within 0-100 % */
        *b = 1000;
        *a = *b - 4 * step;
    }
    return step;
}

static bool is_indoor(const lv_chart_series_t *ser)
{
    for (int q = 0; q < N_Q; q++) {
        if (ser != NULL && ser == s_v.ser[IN][q]) {
            return true;
        }
    }
    return false;
}

/* the chart's draw events: each line segment's width by its series, the
 * axis labels in the quantity's colour and unit */
static void draw_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->class_p != &lv_chart_class) {
        return;
    }
    if (dsc->type == LV_CHART_DRAW_PART_LINE_AND_POINT && dsc->line_dsc != NULL) {
        /* the same line descriptor serves every series: set it each time */
        dsc->line_dsc->width = is_indoor(dsc->sub_part_ptr) ? UISX(LINE_IN) : UISX(LINE_OUT);
        return;
    }
    if (dsc->type != LV_CHART_DRAW_PART_TICK_LABEL || dsc->text == NULL ||
        (dsc->id != LV_CHART_AXIS_PRIMARY_Y && dsc->id != LV_CHART_AXIS_SECONDARY_Y)) {
        return;
    }
    const int side = dsc->id == LV_CHART_AXIS_SECONDARY_Y;
    const int q = s_v.axis_q[side];
    if (q < 0) {
        dsc->text[0] = '\0';
        return;
    }
    const int32_t v = dsc->value;
    if (q == Q_T) {
        if (s_v.axis_step[side] % 10 == 0) {
            lv_snprintf(dsc->text, dsc->text_length, "%ld\xC2\xB0", (long)(v / 10));
        } else {
            char t[12];
            tenths(t, sizeof(t), v);
            lv_snprintf(dsc->text, dsc->text_length, "%s\xC2\xB0", t);
        }
    } else if (q == Q_H) {
        lv_snprintf(dsc->text, dsc->text_length, "%ld%%", (long)(v / 10));
    } else {
        lv_snprintf(dsc->text, dsc->text_length, "%ld", (long)((v + 5) / 10));
    }
    dsc->label_dsc->color = q_colour(q, OUT);
}

/* the x axis: hours (1 day) or days (7 days), and separators at midnight */
static void render_x(uint32_t first, uint32_t last)
{
    int nl = 0, ns = 0;
    const uint32_t span = last - first;
    lv_coord_t y = s_v.cy + s_v.ch + UISY(4);
    for (uint32_t t = (first + 3599) / 3600 * 3600; t <= last; t += 3600) {
        struct tm tm;
        const time_t tt = t;
        localtime_r(&tt, &tm);
        const lv_coord_t x = s_v.cx + (lv_coord_t)((int64_t)(t - first) * (s_v.cw - 1) / span);
        if (tm.tm_hour == 0 && ns < MAX_SEPS && t > first) {
            lv_obj_set_pos(s_v.sep[ns], x, s_v.cy);
            lv_obj_clear_flag(s_v.sep[ns++], LV_OBJ_FLAG_HIDDEN);
        }
        char text[16] = "";
        if (!s_v.week && tm.tm_hour % 3 == 0) {
            snprintf(text, sizeof(text), "%02d", tm.tm_hour);
        } else if (s_v.week && tm.tm_hour == 12) {
            snprintf(text, sizeof(text), "%s", S()->weekday[tm.tm_wday]);
        }
        if (text[0] != '\0' && nl < MAX_XLABELS) {
            lv_obj_t *l = s_v.xlab[nl++];
            lv_label_set_text(l, text);
            lv_obj_set_pos(l, x - text_w(text, lv_obj_get_style_text_font(l, 0)) / 2, y);
            lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (; nl < MAX_XLABELS; nl++) {
        lv_obj_add_flag(s_v.xlab[nl], LV_OBJ_FLAG_HIDDEN);
    }
    for (; ns < MAX_SEPS; ns++) {
        lv_obj_add_flag(s_v.sep[ns], LV_OBJ_FLAG_HIDDEN);
    }
}

static void render_chart(time_t now, const calibration_t *cal)
{
    const int points = s_v.week ? WEEK_POINTS : DAY_POINTS;
    const uint32_t step = s_v.week ? 3600 : READINGS_SLOT_S;
    const uint32_t base = (uint32_t)now / step * step - (uint32_t)(points - 1) * step;

    memset(s_v.acc, 0, sizeof(*s_v.acc));
    memset(s_v.mm_have, 0, sizeof(s_v.mm_have));
    gather_t g ={ .cal = *cal, .base = base, .step = step, .points = points, .acc = s_v.acc };
    readings_store_changed(NULL, base - 1, gather_cb, &g);

    /* who gets which axis: the first on the left, the second on the right,
       a third (pressure) on the right axis's scale */
    int order[N_Q], n_on = 0;
    for (int q = 0; q < N_Q; q++) {
        if (s_v.on & (1u << q)) {
            order[n_on++] = q;
        }
    }
    s_v.axis_q[0] = n_on > 0 ? order[0] : -1;
    s_v.axis_q[1] = n_on > 1 ? order[1] : -1;
    const int third = n_on > 2 ? order[2] : -1;

    /* each quantity's data range */
    int32_t lo[N_Q] = { 0 }, hi[N_Q] = { 0 };
    bool any[N_Q] = { false };
    for (int q = 0; q < N_Q; q++) {
        for (int w = 0; w < N_WHERE; w++) {
            if (!(s_v.where_on & (1u << w))) {
                continue;   /* the axes fit what's shown */
            }
            for (int i = 0; i < points; i++) {
                if (s_v.acc->n[w][q][i] == 0) {
                    continue;
                }
                const int32_t v = s_v.acc->sum[w][q][i] / s_v.acc->n[w][q][i];
                if (!any[q] || v < lo[q]) {
                    lo[q] = v;
                }
                if (!any[q] || v > hi[q]) {
                    hi[q] = v;
                }
                any[q] = true;
            }
        }
    }
    int32_t ra[2] = { 0, 0 }, rb[2] = { 100, 100 };
    for (int side = 0; side < 2; side++) {
        const int q = s_v.axis_q[side];
        if (q >= 0) {
            s_v.axis_step[side] = nice_range(q, any[q], lo[q], hi[q], &ra[side], &rb[side]);
            lv_chart_set_range(s_v.chart, side ? LV_CHART_AXIS_SECONDARY_Y : LV_CHART_AXIS_PRIMARY_Y,
                               (lv_coord_t)ra[side], (lv_coord_t)rb[side]);
        }
    }
    int32_t pa = 0, pb = 1;   /* the third's own range, mapped onto the right axis */
    if (third >= 0) {
        nice_range(third, any[third], lo[third], hi[third], &pa, &pb);
    }

    lv_chart_set_point_count(s_v.chart, points);
    for (int q = 0; q < N_Q; q++) {
        const int side = s_v.axis_q[1] == q || q == third;
        for (int w = 0; w < N_WHERE; w++) {
            lv_chart_series_t *ser = s_v.ser[w][q];
            if (ser == NULL) {
                continue;
            }
            const bool on = (s_v.on & (1u << q)) && (s_v.where_on & (1u << w));
            ser->y_axis_sec = side;
            lv_chart_hide_series(s_v.chart, ser, !on);
            lv_coord_t *ys = lv_chart_get_y_array(s_v.chart, ser);
            for (int i = 0; i < points; i++) {
                const uint16_t n = s_v.acc->n[w][q][i];
                if (!on || n == 0) {
                    ys[i] = LV_CHART_POINT_NONE;
                    continue;
                }
                int32_t v = s_v.acc->sum[w][q][i] / n;
                if (q == third) {
                    v = ra[1] + (v - pa) * (rb[1] - ra[1]) / (pb - pa);
                }
                ys[i] = (lv_coord_t)v;
            }
        }
    }
    lv_chart_refresh(s_v.chart);

    /* the third's lowest and highest values, by its line's right end */
    for (int k = 0; k < 2; k++) {
        lv_obj_add_flag(s_v.p_lab[k], LV_OBJ_FLAG_HIDDEN);
    }
    if (third >= 0 && any[third]) {
        const int32_t vals[2] = { hi[third], lo[third] };
        for (int k = 0; k < 2; k++) {
            const int32_t mapped = ra[1] + (vals[k] - pa) * (rb[1] - ra[1]) / (pb - pa);
            const lv_coord_t y = s_v.cy + (lv_coord_t)((int64_t)(rb[1] - mapped) * s_v.ch / (rb[1] - ra[1]));
            char t[16];
            snprintf(t, sizeof(t), "%ld hPa", (long)((vals[k] + 5) / 10));
            lv_label_set_text(s_v.p_lab[k], t);
            lv_obj_set_style_text_color(s_v.p_lab[k], q_colour(third, OUT), 0);
            const lv_font_t *f = lv_obj_get_style_text_font(s_v.p_lab[k], 0);
            const lv_coord_t lh = lv_font_get_line_height(f);
            lv_coord_t ly = k == 0 ? y - lh : y;   /* the highest above its level, the lowest below */
            ly = LV_CLAMP(s_v.cy, ly, s_v.cy + s_v.ch - lh);
            lv_obj_set_pos(s_v.p_lab[k], s_v.cx + s_v.cw - text_w(t, f) - UISX(4), ly);
            lv_obj_clear_flag(s_v.p_lab[k], LV_OBJ_FLAG_HIDDEN);
        }
    }
    render_x(base, base + (uint32_t)(points - 1) * step);
}

/* ---- buttons ---- */

/* a button's look: its background, and everything on it (text, icon, line
 * sample) in one colour */
static void style_button(lv_obj_t *b, lv_color_t bg, lv_color_t fg)
{
    lv_obj_set_style_bg_color(b, bg, 0);
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(b); i++) {
        lv_obj_t *c = lv_obj_get_child(b, i);
        if (lv_obj_check_type(c, &lv_label_class)) {
            lv_obj_set_style_text_color(c, fg, 0);
        } else {
            lv_obj_set_style_bg_color(c, fg, 0);   /* the line sample */
        }
    }
}

static void style_buttons(void)
{
    for (int k = 0; k < 2; k++) {
        const bool sel = (k == 1) == s_v.week;
        style_button(s_v.range_btn[k], sel ? COL_ACCENT : COL_ROW, sel ? COL_PANEL : COL_TEXT);
    }
    for (int w = 0; w < N_WHERE; w++) {
        const bool on = (s_v.where_on & (1u << w)) != 0;
        style_button(s_v.where_btn[w], on ? COL_ACCENT : COL_ROW, on ? COL_PANEL : COL_DIM);
    }
    for (int q = 0; q < N_Q; q++) {
        const bool on = (s_v.on & (1u << q)) != 0;
        style_button(s_v.q_btn[q], on ? q_colour(q, OUT) : COL_ROW, on ? lv_color_white() : COL_DIM);
    }
}

static void render(void)
{
    if (s_v.panel == NULL || !s_v.shown) {
        return;
    }
    const time_t now = time(NULL);
    if (now < CLOCK_VALID_FROM) {
        lv_label_set_text(s_v.status, S()->no_time);
        return;
    }
    calibration_t cal;
    calibration_get(&cal);
    render_chart(now, &cal);   /* first: it finds the minimum and maximum the top shows */
    render_top(now, &cal);
}

static void range_cb(lv_event_t *e)
{
    s_v.week = (intptr_t)lv_event_get_user_data(e) == 1;
    style_buttons();
    render();
}

static void where_cb(lv_event_t *e)
{
    const uint8_t bit = 1u << (intptr_t)lv_event_get_user_data(e);
    if (s_v.where_on == bit) {
        return;   /* one of them stays on */
    }
    s_v.where_on ^= bit;
    style_buttons();
    render();
}

static void quantity_cb(lv_event_t *e)
{
    const uint8_t bit = 1u << (intptr_t)lv_event_get_user_data(e);
    if (s_v.on == bit) {
        return;   /* the last one stays on: an empty chart says nothing */
    }
    s_v.on ^= bit;
    style_buttons();
    render();
}

static void timer_cb(lv_timer_t *t)
{
    render();
}

/* sample > 0: a line sample of that thickness before the text, and icon
 * (font_ws_26) after it: the outdoor and indoor buttons double as the
 * chart's legend and carry their place's icon */
static lv_obj_t *button(lv_obj_t *row, const char *text, lv_event_cb_t cb, intptr_t id, lv_coord_t sample,
                        const char *icon)
{
    lv_obj_t *b = lv_btn_create(row);
    lv_obj_set_size(b, LV_SIZE_CONTENT, UISY(44));
    lv_obj_set_style_pad_hor(b, UISX(14), 0);
    lv_obj_set_style_radius(b, UISY(8), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    if (sample > 0) {
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(b, UISX(8), 0);
        lv_obj_t *line = lv_obj_create(b);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, UISX(24), sample);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(line, sample / 2, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }
    if (icon != NULL) {
        lv_obj_t *i = label(b, &font_ws_26, COL_TEXT);
        lv_label_set_text(i, icon);
    }
    lv_obj_t *l = label(b, UIFONT(&font_pl_16, &font_pl_10), COL_TEXT);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)id);
    return b;
}

/* ---- the public part ---- */

void ui_weather_view_build(lv_obj_t *scr, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    const strings_t *str = S();
    const lv_coord_t pad = UISX(12);
    s_v.acc = heap_caps_calloc(1, sizeof(acc_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    lv_obj_t *p = s_v.panel = lv_obj_create(scr);
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_style_bg_color(p, COL_PANEL, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);

    /* the top: a card per place, outdoor from the left, indoor from the
       right; on each its icon and name, the large temperature (100 px
       digits, as on the screensaver), the details beside it in the 26 px
       font and the minimum and maximum under it. Columns sized for the
       widest value, "-12.3°", so nothing moves when the numbers change. */
    const lv_coord_t inset = UISX(12), card_y = UISY(4), card_h = UISY(158), in_card_w = UISX(400);
    const lv_coord_t in_card_x = w - pad - in_card_w;
    const lv_coord_t out_x = pad + inset, out_det_x = out_x + UISX(250);
    const lv_coord_t in_x = in_card_x + inset, in_det_x = in_x + UISX(240);
    for (int k = 0; k < N_WHERE; k++) {
        lv_obj_t *card = lv_obj_create(p);
        lv_obj_remove_style_all(card);
        lv_obj_set_style_bg_color(card, COL_ROW, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, UISY(12), 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(card, k == OUT ? in_card_x - UISX(12) - pad : in_card_w, card_h);
        lv_obj_set_pos(card, k == OUT ? pad : in_card_x, card_y);
    }
    char cap[64];
    s_v.out_cap = label(p, &font_ws_26, COL_DIM);
    snprintf(cap, sizeof(cap), "%s  %s", k_place_icon[OUT], str->where[OUT]);
    lv_label_set_text(s_v.out_cap, cap);
    lv_obj_set_pos(s_v.out_cap, out_x, UISY(8));
    /* the outdoor unit's status beside its caption */
    s_v.status = label(p, &font_pl_16, COL_DIM);
    lv_obj_set_width(s_v.status, in_card_x - UISX(12) - inset - out_det_x);
    lv_label_set_long_mode(s_v.status, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_v.status, out_det_x, UISY(14));
    s_v.out_big = ui_temp_big_create(p, &font_ws_digits_100);   /* its box hugs the digits: they start at its top */
    lv_obj_set_pos(s_v.out_big, out_x, UISY(43));
    for (int k = 0; k < 2; k++) {   /* humidity; pressure */
        s_v.out_det[k] = label(p, &font_ws_26, COL_TEXT);
        lv_obj_set_pos(s_v.out_det[k], out_det_x, UISY(44 + 40 * k));
    }
    s_v.out_mm = label(p, &font_ws_26, COL_DIM);
    lv_label_set_recolor(s_v.out_mm, true);
    lv_obj_set_pos(s_v.out_mm, out_x, UISY(124));

    s_v.in_cap = label(p, &font_ws_26, COL_DIM);
    snprintf(cap, sizeof(cap), "%s  %s", k_place_icon[IN], str->where[IN]);
    lv_label_set_text(s_v.in_cap, cap);
    lv_obj_set_pos(s_v.in_cap, in_x, UISY(8));
    s_v.in_big = ui_temp_big_create(p, &font_ws_digits_100);
    lv_obj_set_pos(s_v.in_big, in_x, UISY(43));
    s_v.in_det = label(p, &font_ws_26, COL_TEXT);
    lv_obj_set_pos(s_v.in_det, in_det_x, UISY(44));
    s_v.in_mm = label(p, &font_ws_26, COL_DIM);
    lv_label_set_recolor(s_v.in_mm, true);
    lv_obj_set_pos(s_v.in_mm, in_x, UISY(124));

    /* the buttons: range on the left, the quantities on the right */
    lv_obj_t *row = lv_obj_create(p);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, w - 2 * pad, UISY(44));
    lv_obj_set_pos(row, pad, UISY(170));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, UISX(6), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    s_v.range_btn[0] = button(row, str->day, range_cb, 0, 0, NULL);
    s_v.range_btn[1] = button(row, str->week, range_cb, 1, 0, NULL);
    for (int k = 0; k < 2; k++) {          /* the groups apart: range | where | what */
        lv_obj_t *gap = lv_obj_create(row);
        lv_obj_remove_style_all(gap);
        lv_obj_set_height(gap, 1);
        lv_obj_set_flex_grow(gap, 1);
        if (k == 0) {
            s_v.where_btn[OUT] = button(row, str->where[OUT], where_cb, OUT, UISY(LINE_OUT), k_place_icon[OUT]);
            s_v.where_btn[IN] = button(row, str->where[IN], where_cb, IN, UISY(LINE_IN), k_place_icon[IN]);
        }
    }
    for (int q = 0; q < N_Q; q++) {
        s_v.q_btn[q] = button(row, str->quantity[q], quantity_cb, q, 0, NULL);
    }

    /* the chart, with room for the axis labels on both sides and the hours below */
    s_v.cx = UISX(AXIS_LABEL_W);
    s_v.cy = UISY(228);
    s_v.cw = w - 2 * UISX(AXIS_LABEL_W);
    s_v.ch = h - s_v.cy - UISY(30);
    lv_obj_t *c = s_v.chart = lv_chart_create(p);
    lv_obj_set_size(c, s_v.cw, s_v.ch);
    lv_obj_set_pos(c, s_v.cx, s_v.cy);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(c, LV_CHART_UPDATE_MODE_CIRCULAR);   /* the arrays are filled by index */
    lv_chart_set_div_line_count(c, 5, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_line_color(c, COL_ROW, LV_PART_MAIN);
    lv_obj_set_style_line_width(c, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(c, UISX(LINE_OUT), LV_PART_ITEMS);   /* per series in draw_cb() */
    lv_obj_set_style_size(c, 0, LV_PART_INDICATOR);   /* no dots on the points */
    lv_obj_set_style_text_font(c, UIFONT(&font_pl_14, &font_pl_8), LV_PART_TICKS);
    lv_obj_set_style_text_color(c, COL_DIM, LV_PART_TICKS);
    lv_obj_set_style_line_width(c, 0, LV_PART_TICKS);
    lv_obj_set_style_pad_left(c, UISX(6), LV_PART_TICKS);
    lv_obj_set_style_pad_right(c, UISX(6), LV_PART_TICKS);
    lv_chart_set_axis_tick(c, LV_CHART_AXIS_PRIMARY_Y, 0, 0, 5, 1, true, UISX(AXIS_LABEL_W));
    lv_chart_set_axis_tick(c, LV_CHART_AXIS_SECONDARY_Y, 0, 0, 5, 1, true, UISX(AXIS_LABEL_W));
    lv_obj_add_event_cb(c, draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_chart_set_point_count(c, DAY_POINTS);
    /* indoor first, so outdoor's lines are drawn on top */
    for (int wh = N_WHERE - 1; wh >= 0; wh--) {
        for (int q = 0; q < N_Q; q++) {
            if (wh == IN && q == Q_P) {
                continue;   /* no indoor pressure */
            }
            s_v.ser[wh][q] = lv_chart_add_series(c, q_colour(q, wh), LV_CHART_AXIS_PRIMARY_Y);
            lv_chart_set_all_value(c, s_v.ser[wh][q], LV_CHART_POINT_NONE);
        }
    }

    for (int i = 0; i < MAX_SEPS; i++) {   /* midnight, over the chart */
        lv_obj_t *s = s_v.sep[i] = lv_obj_create(p);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, 1, s_v.ch);
        lv_obj_set_style_bg_color(s, COL_DIM, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_40, 0);
        lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < MAX_XLABELS; i++) {
        s_v.xlab[i] = label(p, UIFONT(&font_pl_14, &font_pl_8), COL_DIM);
        lv_obj_add_flag(s_v.xlab[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int k = 0; k < 2; k++) {
        s_v.p_lab[k] = label(p, UIFONT(&font_pl_14, &font_pl_8), COL_TEXT);
        lv_obj_add_flag(s_v.p_lab[k], LV_OBJ_FLAG_HIDDEN);
    }

    style_buttons();
    s_v.timer = lv_timer_create(timer_cb, REFRESH_MS, NULL);
    lv_timer_pause(s_v.timer);
}

void ui_weather_view_show(bool shown)
{
    if (s_v.panel == NULL || shown == s_v.shown) {
        return;
    }
    s_v.shown = shown;
    if (shown) {
        lv_obj_clear_flag(s_v.panel, LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(s_v.timer);
        lv_timer_reset(s_v.timer);
        render();
    } else {
        lv_obj_add_flag(s_v.panel, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(s_v.timer);
    }
}

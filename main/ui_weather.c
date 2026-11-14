/*
 * weather-station fork: the weather station's touchscreen UI. For now the
 * "Weather station" tab in the settings: the outdoor unit's diagnostics, the
 * same as the web page /outdoor, refreshed every 5 s while settings are
 * open, and below them the simple settings (interval, indoor sensor,
 * calibration). Each change is saved by itself 2 s after the last tap and
 * applies live: the overlay's Save button (upstream's settings, then a
 * restart) isn't needed for them. Pairing, exact values and the altitude
 * override stay on the web page. Later the weather view, the header text and
 * the screensaver block.
 *
 * Labels follow the display's language (English, Polish). Event texts stay
 * English: they're written when the event happens, also into the log file;
 * Polish ones need structured events, due with the weather view.
 */
#include "ui_weather.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "altitude.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "fonts.h"
#include "outdoor_link.h"
#include "settings.h"
#include "theme.h"
#include "ui_scale.h"
#include "ws_settings.h"

#define COL_PANEL  (app_theme()->panel)
#define COL_ACCENT (app_theme()->accent)
#define COL_TEXT   (app_theme()->text)
#define COL_DIM    (app_theme()->dim)
#define COL_WARN   lv_color_hex(0xffd166)
#define COL_BAD    lv_color_hex(0xff6b6b)

#define REFRESH_MS    5000
#define TAB_EVENTS    8
#define EVENTS_TEXT   (TAB_EVENTS * 300)
#define SAVE_DELAY_MS 2000                 /* after the last tap */
#define MSG_MS        3000                 /* "Saved" stays this long */

/* the steppers, in stored units, shown as value / div */
enum { ST_OUT_T, ST_OUT_RH, ST_IN_T, ST_IN_RH, ST_BAT, N_STEP };
static const struct {
    int16_t step, limit, div;
    const char *unit;
} k_step[N_STEP] = {
    [ST_OUT_T] = { 10, 500, 100, " \xC2\xB0" "C" },
    [ST_OUT_RH] = { 10, 1000, 100, " %" },
    [ST_IN_T] = { 10, 500, 100, " \xC2\xB0" "C" },
    [ST_IN_RH] = { 10, 1000, 100, " %" },
    [ST_BAT] = { 1, 50, 10, " %" },        /* per mille, shown as percent: the fonts have no per-mille sign */
};

/* the touchscreen's choice; the web page can set any value in between */
static const uint16_t k_intervals[] = { 180, 300, 600, 900, 1200, 1800, 3600, 7200, 10800, 21600 };
#define N_INTERVALS (sizeof(k_intervals) / sizeof(k_intervals[0]))

/* counted words, number first as on the web page; Polish needs three forms
 * (1 ramka, 2-4 ramki, 5 ramek), English uses `many` for "few" too */
typedef struct {
    const char *one, *few, *many;
} noun_t;

enum {
    W_READING, W_FAILED, W_RESET, W_BROWNOUT,                        /* the last frame */
    W_FRAME, W_REJECTED, W_UNANSWERED, W_UNCONFIRMED,                /* since the display started */
    W_RECEIVED, W_LATE, W_MISSING, W_TWICE, W_CHANNEL,
    N_WORDS
};

typedef struct {
    const char *tab, *link_off;
    const char *status[4];                 /* in outdoor_status_t order */
    const char *last_heard, *next, *overdue; /* %s: a duration */
    const char *battery, *no_reading, *low, *signal, *sensors;
    const char *firmware, *build, *clock, *synced, *not_set, *channel, *space;
    const char *frame, *started, *overflow, *counts, *events, *no_events;
    noun_t word[N_WORDS];
    bool slavic;                           /* Polish plural forms */
    const char *reset[16];                 /* esp_reset_reason_t */
    /* the settings part */
    const char *settings, *every, *hours, *indoor, *offsets, *step[N_STEP], *cal_hint;
    const char *alt_home, *alt_set, *alt_none, *web, *saved, *not_saved;   /* %d: metres; %s: why */
} strings_t;

/* frame and counts take the same arguments, in the same order, in both: the
 * counted words come ready as "N word" */
static const strings_t k_en = {
    .tab = "Weather station",
    .link_off = "The link to the outdoor unit is off (switched off or not paired): see the weather station page on the web panel.",
    .status = { "not heard yet", "OK", "late", "silent" },
    .last_heard = " · last heard %s ago",
    .next = " · next frame in %s",
    .overdue = " · overdue by %s",
    .battery = "Battery", .no_reading = "no reading", .low = " (low)",
    .signal = "Signal", .sensors = "Sensors",
    .firmware = "Firmware", .build = "build %u",
    .clock = "Clock", .synced = "synced", .not_set = "not set", .channel = "Channel",
    .space = "Free space",
    .frame = "Last frame: %s · since its previous frame: awake %lu ms, %s, %s, %s",
    .started = " · started by: %s",
    .overflow = " · its backlog overflowed",
    .counts = "Since the display started (%s): %s, %s, %s, %s\n%s, %s, %s, %s · %s",
    .events = "Recent events",
    .no_events = "none yet",
    .word = {
        [W_READING] = { "reading", "readings", "readings" },
        [W_FAILED] = { "failed search", "failed searches", "failed searches" },
        [W_RESET] = { "reset", "resets", "resets" },
        [W_BROWNOUT] = { "brownout", "brownouts", "brownouts" },
        [W_FRAME] = { "frame", "frames", "frames" },
        [W_REJECTED] = { "rejected", "rejected", "rejected" },
        [W_UNANSWERED] = { "unanswered", "unanswered", "unanswered" },
        [W_UNCONFIRMED] = { "reply unconfirmed", "replies unconfirmed", "replies unconfirmed" },
        [W_RECEIVED] = { "reading", "readings", "readings" },
        [W_LATE] = { "late", "late", "late" },
        [W_MISSING] = { "missing", "missing", "missing" },
        [W_TWICE] = { "twice", "twice", "twice" },
        [W_CHANNEL] = { "channel change", "channel changes", "channel changes" },
    },
    .reset = { "unknown", "power-on", "reset pin", "software restart", "panic",
               "interrupt watchdog", "task watchdog", "other watchdog", "deep-sleep wake",
               "brownout", "SDIO", "USB", "JTAG", "eFuse", "power glitch", "CPU lockup" },
    .settings = "Settings",
    .every = "Measures every",
    .hours = "h",
    .indoor = "Indoor sensor",
    .offsets = "Offsets",
    .step = { "Outdoor temperature", "Outdoor humidity", "Indoor temperature", "Indoor humidity",
              "Battery voltage" },
    .cal_hint = "Added to what the sensors measure; the whole history follows. "
                "Changes are saved by themselves.",
    .alt_home = "Altitude %d m, from the home location: pressure is shown at sea level.",
    .alt_set = "Altitude %d m, set on the web page: pressure is shown at sea level.",
    .alt_none = "No altitude (the home location comes from the internet address): "
                "pressure is shown as measured.",
    .web = "Pairing, exact values and the altitude: the weather station page on the web panel (/outdoor).",
    .saved = "Saved",
    .not_saved = "Not saved: %s",
};

static const strings_t k_pl = {
    .tab = "Stacja pogodowa",
    .link_off = "Połączenie z jednostką zewnętrzną wyłączone (wyłączone lub brak parowania): zobacz stronę stacji pogodowej w panelu WWW.",
    .status = { "jeszcze nie odebrano", "OK", "spóźnia się", "milczy" },
    .last_heard = " · ostatnio %s temu",
    .next = " · następna ramka za %s",
    .overdue = " · spóźnienie %s",
    .battery = "Bateria", .no_reading = "brak odczytu", .low = " (niska)",
    .signal = "Sygnał", .sensors = "Czujniki",
    .firmware = "Oprogramowanie", .build = "wersja %u",
    .clock = "Zegar", .synced = "zsynchronizowany", .not_set = "nieustawiony", .channel = "Kanał",
    .space = "Wolne miejsce",
    .frame = "Ostatnia ramka: %s · od poprzedniej: czas pracy %lu ms, %s, %s, %s",
    .started = " · uruchomienie: %s",
    .overflow = " · bufor się przepełnił",
    .counts = "Od startu wyświetlacza (%s): %s, %s, %s, %s\n%s, %s, %s, %s · %s",
    .events = "Ostatnie zdarzenia",
    .no_events = "brak",
    .slavic = true,
    .word = {
        [W_READING] = { "odczyt", "odczyty", "odczytów" },
        [W_FAILED] = { "nieudane wyszukiwanie", "nieudane wyszukiwania", "nieudanych wyszukiwań" },
        [W_RESET] = { "reset", "resety", "resetów" },
        [W_BROWNOUT] = { "spadek napięcia", "spadki napięcia", "spadków napięcia" },
        [W_FRAME] = { "ramka", "ramki", "ramek" },
        [W_REJECTED] = { "odrzucona", "odrzucone", "odrzuconych" },
        [W_UNANSWERED] = { "bez odpowiedzi", "bez odpowiedzi", "bez odpowiedzi" },
        [W_UNCONFIRMED] = { "niepotwierdzona odpowiedź", "niepotwierdzone odpowiedzi",
                            "niepotwierdzonych odpowiedzi" },
        [W_RECEIVED] = { "odczyt", "odczyty", "odczytów" },
        [W_LATE] = { "spóźniony", "spóźnione", "spóźnionych" },
        [W_MISSING] = { "brakujący", "brakujące", "brakujących" },
        [W_TWICE] = { "podwójny", "podwójne", "podwójnych" },
        [W_CHANNEL] = { "zmiana kanału", "zmiany kanału", "zmian kanału" },
    },
    .reset = { "nieznany", "włączenie zasilania", "przycisk reset", "restart programowy",
               "błąd krytyczny", "watchdog przerwań", "watchdog zadań", "inny watchdog",
               "wybudzenie", "spadek napięcia", "SDIO", "USB", "JTAG", "eFuse",
               "zakłócenie zasilania", "zawieszenie CPU" },
    .settings = "Ustawienia",
    .every = "Pomiar co",
    .hours = "godz.",
    .indoor = "Czujnik wewnętrzny",
    .offsets = "Korekcje",
    .step = { "Temperatura zewnątrz", "Wilgotność zewnątrz", "Temperatura wewnątrz",
              "Wilgotność wewnątrz", "Napięcie baterii" },
    .cal_hint = "Dodawane do pomiarów czujników; obejmują też całą historię. "
                "Zmiany zapisują się same.",
    .alt_home = "Wysokość %d m z lokalizacji domowej: ciśnienie pokazywane na poziomie morza.",
    .alt_set = "Wysokość %d m ustawiona na stronie WWW: ciśnienie pokazywane na poziomie morza.",
    .alt_none = "Brak wysokości (lokalizacja domowa z adresu internetowego): "
                "ciśnienie pokazywane tak, jak zmierzone.",
    .web = "Parowanie, dokładne wartości i wysokość: strona stacji pogodowej w panelu WWW (/outdoor).",
    .saved = "Zapisano",
    .not_saved = "Nie zapisano: %s",
};

static const strings_t *S(void)
{
    return settings_get()->lang == 1 ? &k_pl : &k_en;
}

/* the settings tab's widgets; one tab at a time */
static struct {
    lv_obj_t *status, *bat, *sig, *sensors, *fw, *clock, *chan, *space, *frame, *counts, *events;
    lv_timer_t *timer;
    outdoor_event_t *ev;                   /* TAB_EVENTS, then EVENTS_TEXT chars; PSRAM */
    char *text;
    /* the settings part: the values as edited, saved SAVE_DELAY_MS after the last change */
    lv_obj_t *step_val[N_STEP], *msg;
    lv_timer_t *save_timer, *msg_timer;
    int16_t step_v[N_STEP];
    uint16_t intervals[N_INTERVALS + 1];   /* the choice, plus the current value if it isn't one */
    uint8_t n_intervals;
    uint16_t interval_s;
    bool indoor_on, dirty;
} s_tab;

const char *ui_weather_tab_name(void)
{
    return S()->tab;
}

static void append(char *buf, size_t size, const char *fmt, ...)
{
    const size_t len = strlen(buf);
    if (len + 1 >= size) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + len, size - len, fmt, ap);
    va_end(ap);
}

/* "N word" with the word's form for N */
static void count(char *buf, size_t size, unsigned long n, int word)
{
    const noun_t *w = &S()->word[word];
    const char *form = w->many;
    if (n == 1) {
        form = w->one;
    } else if (S()->slavic && n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14)) {
        form = w->few;
    }
    snprintf(buf, size, "%lu %s", n, form);
}

static void dur(char *buf, size_t n, int64_t s)
{
    if (s < 0) {
        s = 0;
    }
    if (s < 90) {
        snprintf(buf, n, "%d s", (int)s);
    } else if (s < 5400) {
        snprintf(buf, n, "%d min", (int)((s + 30) / 60));
    } else {
        snprintf(buf, n, "%d h %d min", (int)(s / 3600), (int)(s % 3600 / 60));
    }
}

static void refresh(lv_timer_t *timer)
{
    const strings_t *str = S();
    outdoor_health_t h;
    outdoor_link_health(&h);
    char buf[400], d[24];

    if (!h.running) {
        lv_label_set_text(s_tab.status, str->link_off);
        lv_obj_set_style_text_color(s_tab.status, COL_DIM, 0);
        return;
    }

    snprintf(buf, sizeof(buf), "%s", str->status[h.status]);
    if (h.last_heard_us != 0) {
        const int64_t ago = (esp_timer_get_time() - h.last_heard_us) / 1000000;
        const int64_t due = (int64_t)h.expected_gap_s - ago;
        dur(d, sizeof(d), ago);
        append(buf, sizeof(buf), str->last_heard, d);
        dur(d, sizeof(d), due >= 0 ? due : -due);
        append(buf, sizeof(buf), due >= 0 ? str->next : str->overdue, d);
    }
    lv_label_set_text(s_tab.status, buf);
    lv_obj_set_style_text_color(s_tab.status,
                                h.status == OUTDOOR_OK     ? COL_ACCENT :
                                h.status == OUTDOOR_LATE   ? COL_WARN :
                                h.status == OUTDOOR_SILENT ? COL_BAD : COL_DIM, 0);

    if (h.last_heard_us != 0) {
        if (h.bat_mv == 0xFFFF) {
            lv_label_set_text(s_tab.bat, str->no_reading);
        } else {
            snprintf(buf, sizeof(buf), "%u.%02u V%s", h.bat_mv / 1000, (h.bat_mv % 1000) / 10,
                     h.low_battery ? str->low : "");
            lv_label_set_text(s_tab.bat, buf);
        }
        snprintf(buf, sizeof(buf), "%d dBm", h.rssi);
        lv_label_set_text(s_tab.sig, buf);
        snprintf(buf, sizeof(buf), "#%s SHT40 %s#  #%s BME280 %s#",
                 h.sht_ok ? "5dd39e" : "ff6b6b", h.sht_ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE,
                 h.bme_ok ? "5dd39e" : "ff6b6b", h.bme_ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
        lv_label_set_text(s_tab.sensors, buf);
        snprintf(buf, sizeof(buf), str->build, h.fw_build);
        lv_label_set_text(s_tab.fw, buf);
        snprintf(buf, sizeof(buf), "%s", h.clock_synced ? str->synced : str->not_set);
        if (h.offset_known) {
            append(buf, sizeof(buf), ", %+lld s", (long long)h.clock_offset_s);
        }
        lv_label_set_text(s_tab.clock, buf);

        char w[4][48];
        count(w[0], sizeof(w[0]), h.records, W_READING);
        count(w[1], sizeof(w[1]), h.failed_searches, W_FAILED);
        count(w[2], sizeof(w[2]), h.resets, W_RESET);
        count(w[3], sizeof(w[3]), h.brownouts, W_BROWNOUT);
        snprintf(buf, sizeof(buf), str->frame, w[0], (unsigned long)h.awake_total_ms, w[1], w[2], w[3]);
        if (h.reset_reason != 8) {        /* not a wake from deep sleep */
            append(buf, sizeof(buf), str->started,
                   h.reset_reason < 16 ? str->reset[h.reset_reason] : "?");
        }
        if (h.backlog_dropped) {
            append(buf, sizeof(buf), "%s", str->overflow);
        }
        lv_label_set_text(s_tab.frame, buf);
    }
    if (h.channel != 0) {
        snprintf(buf, sizeof(buf), "%u", h.channel);
        lv_label_set_text(s_tab.chan, buf);
    }
    if (h.assets_free_kb >= 0) {
        snprintf(buf, sizeof(buf), h.assets_free_kb < 100 ? "#ff6b6b %ld KB#" : "%ld KB",
                 (long)h.assets_free_kb);
        lv_label_set_text(s_tab.space, buf);
    }

    dur(d, sizeof(d), h.uptime_s);
    const unsigned long n[9] = { h.frames, h.rejected, h.unanswered, h.replies_unacked, h.readings,
                                 h.late, h.missing, h.twice, h.channel_changes };
    char c[9][48];
    for (int i = 0; i < 9; i++) {
        count(c[i], sizeof(c[i]), n[i], W_FRAME + i);
    }
    snprintf(buf, sizeof(buf), str->counts, d, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8]);
    lv_label_set_text(s_tab.counts, buf);

    if (s_tab.ev != NULL) {
        const size_t n = outdoor_link_events(s_tab.ev, TAB_EVENTS);
        s_tab.text[0] = '\0';
        for (size_t i = 0; i < n; i++) {
            char t[8] = "--:--";
            if (s_tab.ev[i].unix_time != 0) {
                const time_t when = (time_t)s_tab.ev[i].unix_time;
                struct tm tm;
                localtime_r(&when, &tm);
                snprintf(t, sizeof(t), "%02d:%02d", tm.tm_hour, tm.tm_min);
            }
            append(s_tab.text, EVENTS_TEXT, "%s%s  %s  %s", i ? "\n" : "", t,
                   s_tab.ev[i].kind, s_tab.ev[i].text);
        }
        lv_label_set_text(s_tab.events, n ? s_tab.text : str->no_events);
    }
}

/* The edited values into the settings, keeping everything the tab doesn't
 * show (a change on the web page meanwhile stays). */
static void save_now(void)
{
    if (!s_tab.dirty) {
        return;
    }
    s_tab.dirty = false;
    ws_settings_t cfg;
    ws_settings_get(&cfg);
    cfg.interval_s = s_tab.interval_s;
    cfg.indoor_on = s_tab.indoor_on;
    cfg.cal_out_t = s_tab.step_v[ST_OUT_T];
    cfg.cal_out_rh = s_tab.step_v[ST_OUT_RH];
    cfg.cal_in_t = s_tab.step_v[ST_IN_T];
    cfg.cal_in_rh = s_tab.step_v[ST_IN_RH];
    cfg.bat_trim_pm = s_tab.step_v[ST_BAT];
    bool restart;                          /* never: the pairing isn't touched here */
    const char *why = NULL;
    const bool ok = ws_settings_set(&cfg, &restart, &why) == ESP_OK;
    if (s_tab.msg != NULL) {
        char buf[96];
        snprintf(buf, sizeof(buf), ok ? S()->saved : S()->not_saved, why != NULL ? why : "?");
        lv_label_set_text(s_tab.msg, buf);
        lv_obj_set_style_text_color(s_tab.msg, ok ? COL_ACCENT : COL_BAD, 0);
        lv_timer_reset(s_tab.msg_timer);
        lv_timer_resume(s_tab.msg_timer);
    }
}

static void save_timer_cb(lv_timer_t *t)
{
    lv_timer_pause(t);
    save_now();
}

static void msg_timer_cb(lv_timer_t *t)
{
    lv_timer_pause(t);
    lv_label_set_text(s_tab.msg, "");
}

/* a change: saved once the taps stop */
static void changed(void)
{
    s_tab.dirty = true;
    lv_timer_reset(s_tab.save_timer);
    lv_timer_resume(s_tab.save_timer);
}

static void show_step(int i)
{
    char buf[24];
    const int16_t v = s_tab.step_v[i];
    /* a value set on the web page between the steps shows its second decimal */
    snprintf(buf, sizeof(buf), v % k_step[i].step == 0 ? "%+.1f%s" : "%+.2f%s",
             (double)v / k_step[i].div, k_step[i].unit);
    lv_label_set_text(s_tab.step_val[i], buf);
}

/* user data: the stepper's index, times 2, plus 1 for up */
static void step_cb(lv_event_t *e)
{
    const uintptr_t d = (uintptr_t)lv_event_get_user_data(e);
    const int i = (int)(d >> 1);
    int v = s_tab.step_v[i] + ((d & 1) ? k_step[i].step : -k_step[i].step);
    v = v > k_step[i].limit ? k_step[i].limit : v < -k_step[i].limit ? -k_step[i].limit : v;
    if (v != s_tab.step_v[i]) {
        s_tab.step_v[i] = (int16_t)v;
        show_step(i);
        changed();
    }
}

static void interval_cb(lv_event_t *e)
{
    const uint16_t i = lv_dropdown_get_selected(lv_event_get_target(e));
    if (i < s_tab.n_intervals) {
        s_tab.interval_s = s_tab.intervals[i];
        changed();
    }
}

static void indoor_cb(lv_event_t *e)
{
    s_tab.indoor_on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    changed();
}

static void on_delete(lv_event_t *e)
{
    s_tab.msg = NULL;                      /* going away with the tab */
    save_now();                            /* a change still waiting for its 2 s */
    if (s_tab.timer != NULL) {
        lv_timer_del(s_tab.timer);
    }
    if (s_tab.save_timer != NULL) {
        lv_timer_del(s_tab.save_timer);
    }
    if (s_tab.msg_timer != NULL) {
        lv_timer_del(s_tab.msg_timer);
    }
    heap_caps_free(s_tab.ev);
    memset(&s_tab, 0, sizeof(s_tab));
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

/* a small panel with a title; returns its value label */
static lv_obj_t *card(lv_obj_t *row, const char *title)
{
    lv_obj_t *c = lv_obj_create(row);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(c, UISX(120), 0);
    lv_obj_set_style_bg_color(c, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, UISX(10), 0);
    lv_obj_set_style_pad_hor(c, UISX(14), 0);
    lv_obj_set_style_pad_ver(c, UISY(8), 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(c);
    lv_obj_set_style_text_font(t, UIFONT(&font_pl_14, &font_pl_8), 0);
    lv_obj_set_style_text_color(t, COL_DIM, 0);
    lv_label_set_text(t, title);

    lv_obj_t *v = lv_label_create(c);
    lv_obj_set_style_text_font(v, UIFONT(&font_pl_20, &font_pl_12), 0);
    lv_obj_set_style_text_color(v, COL_TEXT, 0);
    lv_label_set_recolor(v, true);
    lv_label_set_text(v, "-");
    return v;
}

static lv_obj_t *flex_box(lv_obj_t *parent, lv_coord_t w, lv_flex_flow_t flow)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(b, flow);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(b, UISX(10), 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

/* a row of two halves, outdoor on the left, indoor on the right; on a
 * narrower screen than the 7B's the right half wraps below */
static lv_obj_t *settings_row(lv_obj_t *box)
{
    return flex_box(box, LV_PCT(100), LV_FLEX_FLOW_ROW_WRAP);
}

static lv_obj_t *half(lv_obj_t *row)
{
    lv_obj_t *cell = flex_box(row, LV_SIZE_CONTENT, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_min_width(cell, LV_PCT(48), 0);
    return cell;
}

static void row_label(lv_obj_t *cell, const char *title)
{
    lv_obj_t *l = lv_label_create(cell);
    lv_obj_set_width(l, UISX(210));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, UIFONT(&font_pl_16, &font_pl_10), 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_label_set_text(l, title);
}

static void step_button(lv_obj_t *cell, int i, bool up)
{
    lv_obj_t *b = lv_btn_create(cell);
    lv_obj_set_size(b, UISX(52), UISY(44));
    lv_obj_set_style_bg_color(b, COL_PANEL, 0);
    void *d = (void *)(uintptr_t)(i << 1 | (up ? 1 : 0));
    /* a tap is one step; held, it keeps stepping */
    lv_obj_add_event_cb(b, step_cb, LV_EVENT_SHORT_CLICKED, d);
    lv_obj_add_event_cb(b, step_cb, LV_EVENT_LONG_PRESSED, d);
    lv_obj_add_event_cb(b, step_cb, LV_EVENT_LONG_PRESSED_REPEAT, d);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, UIFONT(&font_pl_20, &font_pl_12), 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_label_set_text(l, up ? "+" : "-");
    lv_obj_center(l);
}

static void stepper(lv_obj_t *cell, int i)
{
    row_label(cell, S()->step[i]);
    step_button(cell, i, false);
    lv_obj_t *v = lv_label_create(cell);
    lv_obj_set_width(v, UISX(110));
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(v, UIFONT(&font_pl_20, &font_pl_12), 0);
    lv_obj_set_style_text_color(v, COL_TEXT, 0);
    s_tab.step_val[i] = v;
    show_step(i);
    step_button(cell, i, true);
}

static void interval_text(char *buf, size_t n, uint16_t s)
{
    const unsigned min = (s + 30) / 60;
    if (min < 60) {
        snprintf(buf, n, "%u min", min);
    } else if (min % 60 == 0) {
        snprintf(buf, n, "%u %s", min / 60, S()->hours);
    } else {
        snprintf(buf, n, "%u %s %u min", min / 60, S()->hours, min % 60);
    }
}

static lv_obj_t *interval_dropdown(lv_obj_t *cell)
{
    /* the choice, with the current value slotted in when the web page (or
     * the bench's 1 minute) set one that isn't on it */
    s_tab.n_intervals = 0;
    bool placed = false;
    for (size_t k = 0; k < N_INTERVALS; k++) {
        if (!placed && s_tab.interval_s < k_intervals[k]) {
            s_tab.intervals[s_tab.n_intervals++] = s_tab.interval_s;
        }
        placed = placed || s_tab.interval_s <= k_intervals[k];
        s_tab.intervals[s_tab.n_intervals++] = k_intervals[k];
    }
    if (!placed) {
        s_tab.intervals[s_tab.n_intervals++] = s_tab.interval_s;
    }
    char options[N_INTERVALS * 20], one[20];
    options[0] = '\0';
    uint16_t selected = 0;
    for (uint8_t k = 0; k < s_tab.n_intervals; k++) {
        interval_text(one, sizeof(one), s_tab.intervals[k]);
        append(options, sizeof(options), "%s%s", k ? "\n" : "", one);
        if (s_tab.intervals[k] == s_tab.interval_s) {
            selected = k;
        }
    }

    lv_obj_t *dd = lv_dropdown_create(cell);
    lv_obj_set_size(dd, UISX(170), UISY(44));
    lv_obj_set_style_bg_color(dd, COL_PANEL, 0);
    lv_obj_set_style_text_color(dd, COL_TEXT, 0);
    lv_obj_set_style_text_font(dd, UIFONT(&font_pl_16, &font_pl_10), 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_text_font(list, UIFONT(&font_pl_16, &font_pl_10), 0);
    lv_obj_set_style_bg_color(list, COL_PANEL, 0);
    lv_obj_set_style_text_color(list, COL_TEXT, 0);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, selected);
    lv_obj_add_event_cb(dd, interval_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return dd;
}

static void settings_part(lv_obj_t *box)
{
    const strings_t *str = S();
    ws_settings_t cfg;
    ws_settings_get(&cfg);
    s_tab.interval_s = cfg.interval_s;
    s_tab.indoor_on = cfg.indoor_on;
    s_tab.step_v[ST_OUT_T] = cfg.cal_out_t;
    s_tab.step_v[ST_OUT_RH] = cfg.cal_out_rh;
    s_tab.step_v[ST_IN_T] = cfg.cal_in_t;
    s_tab.step_v[ST_IN_RH] = cfg.cal_in_rh;
    s_tab.step_v[ST_BAT] = cfg.bat_trim_pm;

    lv_obj_t *head = text(box, UIFONT(&font_pl_16, &font_pl_10), COL_ACCENT);
    lv_obj_set_style_pad_top(head, UISY(14), 0);
    lv_label_set_text(head, str->settings);

    lv_obj_t *row = settings_row(box);
    lv_obj_t *cell = half(row);
    row_label(cell, str->every);
    interval_dropdown(cell);
    cell = half(row);
    row_label(cell, str->indoor);
    lv_obj_t *sw = lv_switch_create(cell);
    if (s_tab.indoor_on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, indoor_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *sub = text(box, UIFONT(&font_pl_16, &font_pl_10), COL_TEXT);
    lv_obj_set_style_pad_top(sub, UISY(8), 0);
    lv_label_set_text(sub, str->offsets);
    row = settings_row(box);
    stepper(half(row), ST_OUT_T);
    stepper(half(row), ST_IN_T);
    row = settings_row(box);
    stepper(half(row), ST_OUT_RH);
    stepper(half(row), ST_IN_RH);
    row = settings_row(box);
    stepper(half(row), ST_BAT);

    lv_label_set_text(text(box, UIFONT(&font_pl_14, &font_pl_8), COL_DIM), str->cal_hint);
    char buf[160];
    int m;
    if (!altitude_m(&cfg, &m)) {
        snprintf(buf, sizeof(buf), "%s", str->alt_none);
    } else {
        snprintf(buf, sizeof(buf), cfg.alt_override_m != WS_ALT_AUTO ? str->alt_set : str->alt_home, m);
    }
    lv_label_set_text(text(box, UIFONT(&font_pl_14, &font_pl_8), COL_DIM), buf);
    lv_label_set_text(text(box, UIFONT(&font_pl_14, &font_pl_8), COL_DIM), str->web);
    s_tab.msg = text(box, UIFONT(&font_pl_16, &font_pl_10), COL_ACCENT);

    s_tab.save_timer = lv_timer_create(save_timer_cb, SAVE_DELAY_MS, NULL);
    lv_timer_pause(s_tab.save_timer);
    s_tab.msg_timer = lv_timer_create(msg_timer_cb, MSG_MS, NULL);
    lv_timer_pause(s_tab.msg_timer);
}

void ui_weather_settings_tab(lv_obj_t *page)
{
    const strings_t *str = S();
    memset(&s_tab, 0, sizeof(s_tab));

    lv_obj_t *box = lv_obj_create(page);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, UISY(10), 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    s_tab.status = text(box, UIFONT(&font_pl_20, &font_pl_12), COL_DIM);

    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(row, UISX(10), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    s_tab.bat = card(row, str->battery);
    s_tab.sig = card(row, str->signal);
    s_tab.sensors = card(row, str->sensors);
    s_tab.fw = card(row, str->firmware);
    s_tab.clock = card(row, str->clock);
    s_tab.chan = card(row, str->channel);
    s_tab.space = card(row, str->space);

    s_tab.frame = text(box, UIFONT(&font_pl_14, &font_pl_8), COL_TEXT);
    s_tab.counts = text(box, UIFONT(&font_pl_14, &font_pl_8), COL_TEXT);
    lv_obj_t *title = text(box, UIFONT(&font_pl_16, &font_pl_10), COL_ACCENT);
    lv_label_set_text(title, str->events);
    s_tab.events = text(box, UIFONT(&font_pl_14, &font_pl_8), COL_TEXT);
    settings_part(box);

    s_tab.ev = heap_caps_malloc(TAB_EVENTS * sizeof(outdoor_event_t) + EVENTS_TEXT,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tab.text = s_tab.ev != NULL ? (char *)(s_tab.ev + TAB_EVENTS) : NULL;

    lv_obj_add_event_cb(box, on_delete, LV_EVENT_DELETE, NULL);
    s_tab.timer = lv_timer_create(refresh, REFRESH_MS, NULL);
    refresh(NULL);
}

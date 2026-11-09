/*
 * weather-station fork: the weather station's touchscreen UI. For now the
 * "Weather station" tab in the settings, with the outdoor unit's
 * diagnostics, the same as the web page /outdoor, refreshed every 5 s while
 * settings are open. Later the weather view, the header text and the
 * screensaver block.
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
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "fonts.h"
#include "outdoor_link.h"
#include "settings.h"
#include "theme.h"
#include "ui_scale.h"

#define COL_PANEL  (app_theme()->panel)
#define COL_ACCENT (app_theme()->accent)
#define COL_TEXT   (app_theme()->text)
#define COL_DIM    (app_theme()->dim)
#define COL_WARN   lv_color_hex(0xffd166)
#define COL_BAD    lv_color_hex(0xff6b6b)

#define REFRESH_MS  5000
#define TAB_EVENTS  8
#define EVENTS_TEXT (TAB_EVENTS * 300)

typedef struct {
    const char *tab, *link_off;
    const char *status[4];                 /* in outdoor_status_t order */
    const char *last_heard, *next, *overdue; /* %s: a duration */
    const char *battery, *no_reading, *low, *signal, *sensors;
    const char *firmware, *build, *clock, *synced, *not_set, *channel, *space;
    const char *frame, *started, *overflow, *counts, *events, *no_events;
    const char *reset[16];                 /* esp_reset_reason_t */
} strings_t;

/* frame and counts take the same arguments, in the same order, in both */
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
    .frame = "Last frame: %u readings · since its previous frame: awake %lu ms, "
             "%u failed searches, %u resets, %u brownouts",
    .started = " · started by: %s",
    .overflow = " · its backlog overflowed",
    .counts = "Since the display started (%s): %lu frames, %lu rejected, %lu unanswered, "
              "%lu replies unconfirmed\n%lu readings, %lu late, %lu missing, %lu twice · "
              "%u channel changes",
    .events = "Recent events",
    .no_events = "none yet",
    .reset = { "unknown", "power-on", "reset pin", "software restart", "panic",
               "interrupt watchdog", "task watchdog", "other watchdog", "deep-sleep wake",
               "brownout", "SDIO", "USB", "JTAG", "eFuse", "power glitch", "CPU lockup" },
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
    .frame = "Ostatnia ramka: odczyty %u · od poprzedniej: czas pracy %lu ms, "
             "nieudane wyszukiwania %u, resety %u, spadki napięcia %u",
    .started = " · uruchomienie: %s",
    .overflow = " · bufor się przepełnił",
    .counts = "Od startu wyświetlacza (%s): ramki %lu, odrzucone %lu, bez odpowiedzi %lu, "
              "niepotwierdzone odpowiedzi %lu\nodczyty %lu, spóźnione %lu, brakujące %lu, "
              "podwójne %lu · zmiany kanału %u",
    .events = "Ostatnie zdarzenia",
    .no_events = "brak",
    .reset = { "nieznany", "włączenie zasilania", "przycisk reset", "restart programowy",
               "błąd krytyczny", "watchdog przerwań", "watchdog zadań", "inny watchdog",
               "wybudzenie", "spadek napięcia", "SDIO", "USB", "JTAG", "eFuse",
               "zakłócenie zasilania", "zawieszenie CPU" },
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

        snprintf(buf, sizeof(buf), str->frame, h.records, (unsigned long)h.awake_total_ms,
                 h.failed_searches, h.resets, h.brownouts);
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
    snprintf(buf, sizeof(buf), str->counts, d, (unsigned long)h.frames, (unsigned long)h.rejected,
             (unsigned long)h.unanswered, (unsigned long)h.replies_unacked,
             (unsigned long)h.readings, (unsigned long)h.late, (unsigned long)h.missing,
             (unsigned long)h.twice, h.channel_changes);
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

static void on_delete(lv_event_t *e)
{
    if (s_tab.timer != NULL) {
        lv_timer_del(s_tab.timer);
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

    s_tab.ev = heap_caps_malloc(TAB_EVENTS * sizeof(outdoor_event_t) + EVENTS_TEXT,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tab.text = s_tab.ev != NULL ? (char *)(s_tab.ev + TAB_EVENTS) : NULL;

    lv_obj_add_event_cb(box, on_delete, LV_EVENT_DELETE, NULL);
    s_tab.timer = lv_timer_create(refresh, REFRESH_MS, NULL);
    refresh(NULL);
}

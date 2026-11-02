/*
 * What the display knows about the outdoor unit: the latest frame, counters
 * since the display started, the late/silent status, and an event log in
 * /assets/outdoor.tsv ("Outdoor unit diagnostics" in DISPLAY.md, in the
 * planning notes).
 *
 *  - The link task does all the writing: frames, rejections and the
 *    housekeeping tick. outdoor_link_health() and outdoor_link_events() hand
 *    copies to other tasks, taken under a mutex, never in a critical section
 *    (the 7B's panel needs its interrupts).
 *  - The status runs on uptime, and the reading counters on the outdoor
 *    unit's own clock: a reading's age is outdoor_now - ts, both from that
 *    clock, synced or not. Neither needs the display's time.
 *  - Events wait in RAM until the display has time, then go to the file with
 *    their real time, worked back from uptime: the file never says 1970.
 */
#include "outdoor_health.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "outdoor_link.h"

static const char *TAG = "outdoor_link";

#define LOG_MAX_BYTES  (64 * 1024)      /* then renamed to .old, as upstream's alerts.tsv */
#define EVENTS         32               /* kept in RAM: the newest, and any not yet written */
#define BACKLOG_SPAN_S (2 * 24 * 3600)  /* the outdoor unit's backlog never spans more */
#define SAME_WAKE_S    5                /* frames this close on its clock come from one wake */
#define SENSOR_RUN     3                /* readings in a row before a sensor counts as lost or back */

typedef struct {
    int64_t up_us;                      /* esp_timer time of the event */
    outdoor_event_t ev;
} slot_t;

/* one local day's numbers, for the summary at midnight */
typedef struct {
    bool     dated;                     /* the display has time, so the day has a date */
    bool     partial;                   /* began at the display's boot, not at midnight */
    int      year, yday;
    time_t   from;
    uint32_t frames, unanswered, readings, late, missing, twice;
    uint32_t failed_searches, resets, brownouts, sht_missing, bme_missing;
    uint64_t awake_ms;
    uint16_t bat_min, bat_max;          /* bat_max 0: no battery reading yet */
    int8_t   rssi_min, rssi_max;
} day_t;

static SemaphoreHandle_t s_mux;         /* guards s_h and the event ring */
static outdoor_health_t s_h;
static slot_t *s_ev;                    /* EVENTS slots in PSRAM; NULL: serial log only */
static unsigned s_ev_next, s_ev_count, s_ev_unwritten;
static volatile uint32_t s_unacked;     /* from the send callback */
static uint32_t s_interval_s;

/* the link task's own */
static day_t s_day;
static bool s_have_ts;
static uint32_t s_last_ts;              /* newest reading counted, outdoor clock (0 is a real
                                           time on it, right after a power-on) */
static uint32_t s_last_now;             /* outdoor_now of the previous frame */
static uint16_t s_last_seq;             /* and its seq */
static bool s_rejecting;                /* inside a burst of rejected frames */
static bool s_sht_ok = true, s_bme_ok = true;
static uint8_t s_sht_run, s_bme_run;
static bool s_file_warned;

static bool have_time(time_t *now)
{
    *now = time(NULL);
    return *now >= CLOCK_VALID_FROM;
}

/* the wall time of an earlier uptime moment */
static time_t wall_at(int64_t up_us, time_t now, int64_t now_us)
{
    return now - (time_t)((now_us - up_us) / 1000000);
}

static void hhmm(char *buf, size_t n, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

static void volts(char *buf, size_t n, uint16_t mv)
{
    if (mv == WS_LINK_NO_U16) {
        strlcpy(buf, "unknown", n);
    } else {
        snprintf(buf, n, "%u.%02u V", mv / 1000, (mv % 1000) / 10);
    }
}

static const char *reset_name(uint8_t reason)   /* esp_reset_reason_t */
{
    static const char *const names[] = {
        "unknown", "power-on", "reset pin", "software restart", "panic",
        "interrupt watchdog", "task watchdog", "other watchdog", "deep-sleep wake",
        "brownout", "SDIO", "USB", "JTAG", "eFuse", "power glitch", "CPU lockup",
    };
    return reason < sizeof(names) / sizeof(names[0]) ? names[reason] : "other";
}

static outdoor_status_t status_of(const outdoor_health_t *h, int64_t now_us)
{
    if (h->last_heard_us == 0) {
        return OUTDOOR_NOT_HEARD;
    }
    const int64_t quiet_us = now_us - h->last_heard_us;
    const int64_t gap_us = (int64_t)h->expected_gap_s * 1000000;
    if (quiet_us > 3 * gap_us) {
        return OUTDOOR_SILENT;
    }
    return 2 * quiet_us > 3 * gap_us ? OUTDOOR_LATE : OUTDOOR_OK;
}

static void event(const char *kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void event(const char *kind, const char *fmt, ...)
{
    char text[sizeof(((outdoor_event_t *)0)->text)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "event %s: %s", kind, text);
    if (s_ev == NULL) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    slot_t *e = &s_ev[s_ev_next];
    e->up_us = esp_timer_get_time();
    e->ev.unix_time = 0;
    strlcpy(e->ev.kind, kind, sizeof(e->ev.kind));
    strlcpy(e->ev.text, text, sizeof(e->ev.text));
    s_ev_next = (s_ev_next + 1) % EVENTS;
    if (s_ev_count < EVENTS) {
        s_ev_count++;
    }
    if (s_ev_unwritten < EVENTS) {
        s_ev_unwritten++;
    }
    xSemaphoreGive(s_mux);
}

/* Pending events into the file, once the display has time to date them. */
static void flush_events(void)
{
    time_t now;
    if (s_ev == NULL || s_ev_unwritten == 0 || !have_time(&now)) {
        return;
    }
    struct stat st;
    if (stat(OUTDOOR_LOG_PATH, &st) == 0 && st.st_size > LOG_MAX_BYTES) {
        unlink(OUTDOOR_LOG_OLD_PATH);
        rename(OUTDOOR_LOG_PATH, OUTDOOR_LOG_OLD_PATH);
    }
    FILE *f = fopen(OUTDOOR_LOG_PATH, "a");
    if (f == NULL) {
        if (!s_file_warned) {
            ESP_LOGW(TAG, "can't open %s; events stay in RAM", OUTDOOR_LOG_PATH);
            s_file_warned = true;
        }
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    while (s_ev_unwritten > 0) {
        outdoor_event_t ev;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        slot_t *e = &s_ev[(s_ev_next + EVENTS - s_ev_unwritten) % EVENTS];
        if (e->ev.unix_time == 0) {
            e->ev.unix_time = wall_at(e->up_us, now, now_us);
        }
        ev = e->ev;
        s_ev_unwritten--;
        xSemaphoreGive(s_mux);
        fprintf(f, "%lld\t%s\t%s\n", (long long)ev.unix_time, ev.kind, ev.text);
    }
    fclose(f);
}

static void day_summary(void)
{
    char date[12], from[16] = "", bat[32] = "battery unknown", rssi[32] = "no frames";
    struct tm tm;
    localtime_r(&s_day.from, &tm);
    strftime(date, sizeof(date), "%Y-%m-%d", &tm);
    if (s_day.partial) {
        char t[8];
        hhmm(t, sizeof(t), s_day.from);
        snprintf(from, sizeof(from), " from %s", t);
    }
    if (s_day.bat_max != 0) {
        snprintf(bat, sizeof(bat), "%u.%02u-%u.%02u V", s_day.bat_min / 1000, (s_day.bat_min % 1000) / 10,
                 s_day.bat_max / 1000, (s_day.bat_max % 1000) / 10);
    }
    if (s_day.frames != 0) {
        snprintf(rssi, sizeof(rssi), "%d..%d dBm", s_day.rssi_min, s_day.rssi_max);
    }
    event("day", "%s%s: %lu frames (%lu unanswered), %lu readings: %lu late, %lu missing, %lu twice; "
                 "awake %llu.%llu s; %lu failed searches, %lu resets, %lu brownouts; %s; %s; "
                 "SHT40 missing %lu, BME280 missing %lu",
          date, from, (unsigned long)s_day.frames, (unsigned long)s_day.unanswered,
          (unsigned long)s_day.readings, (unsigned long)s_day.late, (unsigned long)s_day.missing,
          (unsigned long)s_day.twice, (unsigned long long)(s_day.awake_ms / 1000),
          (unsigned long long)(s_day.awake_ms % 1000 / 100),
          (unsigned long)s_day.failed_searches, (unsigned long)s_day.resets,
          (unsigned long)s_day.brownouts, bat, rssi, (unsigned long)s_day.sht_missing,
          (unsigned long)s_day.bme_missing);
    ESP_LOGI(TAG, "link task: %u bytes of stack never used", (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

/* Gives the day's numbers a date once the display has time, and writes the
 * summary when the local date changes. */
static void day_check(void)
{
    time_t now;
    if (!have_time(&now)) {
        return;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    if (!s_day.dated) {
        /* counting began at the display's boot */
        s_day.dated = true;
        s_day.partial = true;
        s_day.year = tm.tm_year;
        s_day.yday = tm.tm_yday;
        s_day.from = now - (time_t)(esp_timer_get_time() / 1000000);
        return;
    }
    if (tm.tm_year == s_day.year && tm.tm_yday == s_day.yday) {
        return;
    }
    day_summary();
    struct tm midnight = tm;
    midnight.tm_hour = midnight.tm_min = midnight.tm_sec = 0;
    midnight.tm_isdst = -1;
    s_day = (day_t){
        .dated = true,
        .year = tm.tm_year,
        .yday = tm.tm_yday,
        .from = mktime(&midnight),
    };
}

static void sensor(const char *name, bool ok, bool *state, uint8_t *run, uint32_t *missing_today)
{
    if (!ok) {
        (*missing_today)++;
    }
    if (ok == *state) {
        *run = 0;
        return;
    }
    if (++*run < SENSOR_RUN) {
        return;
    }
    *state = ok;
    *run = 0;
    if (ok) {
        event("sensor", "%s answers again", name);
    } else {
        event("sensor", "%s missing in %d readings in a row", name, SENSOR_RUN);
    }
}

/* One reading of an answered frame; false if it was counted before. */
static bool reading(const ws_link_record_t *r, uint32_t outdoor_now, uint32_t gap_s,
                    uint32_t *late, uint32_t *missing)
{
    if (s_have_ts && r->ts <= s_last_ts && s_last_ts - r->ts <= BACKLOG_SPAN_S) {
        return false;                   /* the outdoor unit missed our reply and sent it again */
    }
    if (s_have_ts && r->ts > s_last_ts && r->ts - s_last_ts <= BACKLOG_SPAN_S) {
        const uint32_t step = r->ts - s_last_ts;
        if (2 * step > 3 * s_interval_s) {
            *missing += (step + s_interval_s / 2) / s_interval_s - 1;
        }
    }
    /* otherwise the first reading, or a jump beyond the backlog's reach: its
     * clock changed (set after a power-on, say), and counting starts afresh */
    s_have_ts = true;
    s_last_ts = r->ts;

    /* its age when sent, both times from its own clock */
    const uint32_t age = outdoor_now > r->ts ? outdoor_now - r->ts : 0;
    if (2 * age > 2 * gap_s - s_interval_s) {
        (*late)++;
    }
    if (r->bat_mv != WS_LINK_NO_U16) {
        if (s_day.bat_max == 0) {
            s_day.bat_min = s_day.bat_max = r->bat_mv;
        } else if (r->bat_mv < s_day.bat_min) {
            s_day.bat_min = r->bat_mv;
        } else if (r->bat_mv > s_day.bat_max) {
            s_day.bat_max = r->bat_mv;
        }
    }
    sensor("SHT40", r->flags & WS_REC_SHT_OK, &s_sht_ok, &s_sht_run, &s_day.sht_missing);
    sensor("BME280", r->flags & WS_REC_BME_OK, &s_bme_ok, &s_bme_run, &s_day.bme_missing);
    return true;
}

bool outdoor_health_init(uint32_t interval_s)
{
    s_mux = xSemaphoreCreateMutex();
    if (s_mux == NULL) {
        return false;
    }
    s_interval_s = interval_s;
    s_ev = heap_caps_calloc(EVENTS, sizeof(slot_t), MALLOC_CAP_SPIRAM);
    if (s_ev == NULL) {
        ESP_LOGW(TAG, "no PSRAM for the event ring: events go to the serial log only");
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_h.running = true;
    s_h.expected_gap_s = interval_s;
    xSemaphoreGive(s_mux);
    return true;
}

void outdoor_health_frame(const ws_link_readings_t *f, const uint8_t *records, int8_t rssi,
                          bool answered)
{
    const int64_t now_us = esp_timer_get_time();
    time_t now;
    const bool dated = have_time(&now);
    day_check();

    const bool first = s_h.last_heard_us == 0;
    const bool was_silent = status_of(&s_h, now_us) == OUTDOOR_SILENT;
    const int64_t quiet_s = first ? 0 : (now_us - s_h.last_heard_us) / 1000000;
    /* frames of one wake carry consecutive seqs and nearly the same time; right
     * after a power-on its clock is near 0 again, so the time alone can't tell */
    const bool new_wake = first || f->hdr.seq != (uint16_t)(s_last_seq + 1) ||
                          f->outdoor_now < s_last_now || f->outdoor_now - s_last_now > SAME_WAKE_S;
    const bool synced = f->flags & WS_OUTDOOR_CLOCK_SYNCED;
    const bool low = f->flags & WS_OUTDOOR_LOW_BATTERY;
    const bool dropped = f->flags & WS_OUTDOOR_BACKLOG_DROPPED;
    const bool was_low = s_h.low_battery, was_dropped = s_h.backlog_dropped;
    const uint16_t old_fw = s_h.fw_build;
    const uint32_t gap_s = s_interval_s * (low ? 4 : 1);

    uint32_t readings = 0, late = 0, missing = 0, twice = 0;
    ws_link_record_t r = {0};
    for (unsigned i = 0; i < f->count; i++) {
        memcpy(&r, records + i * sizeof(r), sizeof(r));
        if (!answered) {
            continue;                   /* it comes again with the next wake */
        }
        if (reading(&r, f->outdoor_now, gap_s, &late, &missing)) {
            readings++;
        } else {
            twice++;
        }
    }
    /* r is now the newest reading */

    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_h.status = OUTDOOR_OK;
    s_h.last_heard_us = now_us;
    s_h.expected_gap_s = gap_s;
    s_h.fw_build = f->fw_build;
    s_h.reset_reason = f->reset_reason;
    s_h.clock_synced = synced;
    s_h.low_battery = low;
    s_h.backlog_dropped = dropped;
    s_h.sht_ok = r.flags & WS_REC_SHT_OK;
    s_h.bme_ok = r.flags & WS_REC_BME_OK;
    s_h.bat_mv = r.bat_mv;
    s_h.rssi = rssi;
    s_h.records = f->count;
    s_h.offset_known = dated;
    s_h.clock_offset_s = dated ? (int64_t)now - f->outdoor_now : 0;
    s_h.awake_total_ms = f->awake_total_ms;
    s_h.brownouts = f->brownouts;
    s_h.resets = f->resets;
    s_h.failed_searches = f->failed_searches;
    s_h.frames++;
    s_h.unanswered += !answered;
    s_h.readings += readings;
    s_h.late += late;
    s_h.missing += missing;
    s_h.twice += twice;
    xSemaphoreGive(s_mux);

    s_day.frames++;
    s_day.unanswered += !answered;
    s_day.readings += readings;
    s_day.late += late;
    s_day.missing += missing;
    s_day.twice += twice;
    s_day.failed_searches += f->failed_searches;
    s_day.resets += f->resets;
    s_day.brownouts += f->brownouts;
    s_day.awake_ms += f->awake_total_ms;
    if (s_day.frames == 1 || rssi < s_day.rssi_min) {
        s_day.rssi_min = rssi;
    }
    if (s_day.frames == 1 || rssi > s_day.rssi_max) {
        s_day.rssi_max = rssi;
    }
    s_last_now = f->outdoor_now;
    s_last_seq = f->hdr.seq;
    s_rejecting = false;

    char bat[12];
    volts(bat, sizeof(bat), r.bat_mv);
    if (first) {
        event("first", "first frame since the display started: firmware %u, %s, clock %s, battery %s%s",
              f->fw_build, reset_name(f->reset_reason), synced ? "synced" : "not set yet", bat,
              low ? " (low-battery mode)" : "");
    } else if (was_silent) {
        event("back", "heard again after %lld min: %u readings, %u failed searches, awake %lu ms",
              (long long)(quiet_s / 60), f->count, f->failed_searches, (unsigned long)f->awake_total_ms);
    }
    if (!first && f->fw_build != old_fw) {
        event("firmware", "build %u -> %u", old_fw, f->fw_build);
    }
    if (!first && new_wake && f->reset_reason != ESP_RST_DEEPSLEEP) {
        event("reset", "started afresh: %s", reset_name(f->reset_reason));
    }
    if (f->resets != 0) {
        event("reset", "%u resets since it was last heard (panic, watchdog or software)", f->resets);
    }
    if (f->brownouts != 0) {
        event("brownout", "%u brownout resets since it was last heard", f->brownouts);
    }
    if (dropped && !was_dropped) {
        event("backlog", "its backlog was full: the oldest readings were dropped");
    }
    if (!first && low && !was_low) {
        event("battery", "low: radio only every 4th wake (%s)", bat);
    } else if (!first && !low && was_low) {
        event("battery", "back to normal (%s)", bat);
    }
    if (missing != 0) {
        event("missing", "%lu readings never arrived", (unsigned long)missing);
    }
}

void outdoor_health_rejected(const char *why)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_h.rejected++;
    xSemaphoreGive(s_mux);
    if (!s_rejecting) {
        s_rejecting = true;
        event("rejected", "%s", why);
    }
}

void outdoor_health_reply_sent(bool acked)
{
    if (!acked) {
        __atomic_fetch_add(&s_unacked, 1, __ATOMIC_RELAXED);
    }
}

void outdoor_health_tick(void)
{
    const int64_t now_us = esp_timer_get_time();
    day_check();

    const outdoor_status_t status = status_of(&s_h, now_us);
    if (status == OUTDOOR_SILENT && s_h.status != OUTDOOR_SILENT) {
        char since[40];
        time_t now;
        if (have_time(&now)) {
            char t[8];
            hhmm(t, sizeof(t), wall_at(s_h.last_heard_us, now, now_us));
            snprintf(since, sizeof(since), "since %s", t);
        } else {
            snprintf(since, sizeof(since), "for %lld min", (long long)((now_us - s_h.last_heard_us) / 60000000));
        }
        event("silent", "nothing heard %s (a frame was due every %lu s)", since,
              (unsigned long)s_h.expected_gap_s);
    }

    /* the display's channel, only while it's connected: a reconnect scans them all */
    wifi_ap_record_t ap;
    const uint8_t old = s_h.channel;
    const uint8_t ch = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.primary : old;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_h.status = status;
    if (ch != old) {
        s_h.channel = ch;
        s_h.channel_changes += old != 0;
    }
    xSemaphoreGive(s_mux);
    if (old != 0 && ch != old) {
        event("channel", "the display moved from channel %u to %u", old, ch);
    }

    flush_events();
}

void outdoor_link_health(outdoor_health_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_mux == NULL) {
        return;                         /* not paired in this build, or not started */
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_h;
    xSemaphoreGive(s_mux);
    const int64_t now_us = esp_timer_get_time();
    out->replies_unacked = __atomic_load_n(&s_unacked, __ATOMIC_RELAXED);
    out->status = status_of(out, now_us);
    out->uptime_s = (uint32_t)(now_us / 1000000);
}

size_t outdoor_link_events(outdoor_event_t *out, size_t max)
{
    if (s_mux == NULL || s_ev == NULL) {
        return 0;
    }
    time_t now;
    const bool dated = have_time(&now);
    const int64_t now_us = esp_timer_get_time();
    size_t n = 0;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    for (; n < max && n < s_ev_count; n++) {
        const slot_t *e = &s_ev[(s_ev_next + EVENTS - 1 - n) % EVENTS];
        out[n] = e->ev;
        if (out[n].unix_time == 0 && dated) {
            out[n].unix_time = wall_at(e->up_us, now, now_us);
        }
    }
    xSemaphoreGive(s_mux);
    return n;
}

/*
 * ws_alpha: the firmware of the outdoor unit (solar, ESP32-C3). One linear
 * wake cycle, no tasks:
 *
 *   guard (brownout reset? sleep) -> battery -> sensors -> backlog
 *     -> radio (if the battery and the pairing allow it) -> deep sleep
 *
 * The battery comes first: it's read before anything draws current, and it
 * decides whether the radio may start at all. Every reading goes into the
 * backlog; the radio sends the backlog, oldest first, and a record leaves it
 * only once the display has confirmed it. A wake without an answer costs
 * nothing but time: the next one sends the same records again.
 *
 * Updates (ota.c): a reply may offer a new firmware, fetched after the
 * exchange. The new firmware's first wake must reach the display: it keeps
 * trying for up to 30 s, then goes back to the previous firmware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "backlog.h"
#include "battery.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "link.h"
#include "ota.h"
#include "power.h"
#include "sensors.h"
#include "ws_link.h"

static const char *TAG = "ws_alpha";

#define LOW_BATTERY_EVERY  4           /* radio on every n-th wake below 3.4 V */
#define CLOCK_VALID_FROM   1767225600u /* 2026-01-01: earlier means the display has no time yet */
#define PROBATION_MS       30000       /* a new firmware's first wake: this long to reach the display */
#define PROBATION_CAP_MS   40000
#define RETRY_MS           2000        /* between its tries */

/* a counter needs no init: any start value works, also after a power-on */
static RTC_NOINIT_ATTR uint16_t s_seq;
static RTC_DATA_ATTR uint8_t s_low_wakes;

static const char *const k_mode_name[] = {
    [BATTERY_OK] = "ok",
    [BATTERY_LOW] = "low",
    [BATTERY_EMPTY] = "empty, radio off",
};

static void log_record(const ws_link_record_t *r, battery_mode_t mode)
{
    char sht[40] = "missing", bme[56] = "missing", bat[24] = "unknown";
    if (r->flags & WS_REC_SHT_OK) {
        snprintf(sht, sizeof(sht), "%.2f C %.2f %%RH", r->sht_t / 100.0, r->sht_rh / 100.0);
    }
    if (r->flags & WS_REC_BME_OK) {
        snprintf(bme, sizeof(bme), "%.2f C %.2f %%RH %.1f hPa",
                 r->bme_t / 100.0, r->bme_rh / 100.0, r->bme_p / 10.0);
    }
    if (r->bat_mv != WS_LINK_NO_U16) {
        snprintf(bat, sizeof(bat), "%u mV", r->bat_mv);
    }
    ESP_LOGI(TAG, "SHT40 %s | BME280 %s | battery %s (%s) | brownouts %u",
             sht, bme, bat, k_mode_name[mode], power_brownouts());
}

static bool radio_allowed(battery_mode_t mode)
{
    switch (mode) {
    case BATTERY_EMPTY:
        return false;
    case BATTERY_LOW:
        if (++s_low_wakes < LOW_BATTERY_EVERY) {
            ESP_LOGI(TAG, "low battery: radio on every %d. wake, this is %u.", LOW_BATTERY_EVERY, s_low_wakes);
            return false;
        }
        s_low_wakes = 0;
        return true;
    default:
        s_low_wakes = 0;
        return true;
    }
}

/* one READINGS frame from the oldest records; returns its length */
static size_t build_frame(uint8_t *buf, uint16_t seq, battery_mode_t mode, uint16_t *count)
{
    ws_link_readings_t *f = (ws_link_readings_t *)buf;
    *count = backlog_peek(f->rec, WS_LINK_MAX_RECORDS);

    uint8_t flags = 0;
    if (power_clock_synced()) {
        flags |= WS_OUTDOOR_CLOCK_SYNCED;
    }
    if (backlog_dropped()) {
        flags |= WS_OUTDOOR_BACKLOG_DROPPED;
    }
    if (mode == BATTERY_LOW) {
        flags |= WS_OUTDOOR_LOW_BATTERY;
    }
    if (ota_gave_up()) {
        flags |= WS_OUTDOOR_OTA_FAILED;
    }

    f->hdr.magic = WS_LINK_MAGIC;
    f->hdr.version = WS_LINK_VERSION;
    f->hdr.type = WS_LINK_READINGS;
    f->hdr.seq = seq;
    f->outdoor_now = (uint32_t)time(NULL);
    f->clock_set_at = power_clock_set_at();
    f->fw_build = (uint16_t)atoi(esp_app_get_description()->version);
    f->reset_reason = (uint8_t)esp_reset_reason();
    f->flags = flags;
    f->awake_total_ms = power_awake_total_ms();
    f->brownouts = power_brownouts();
    f->resets = power_resets();
    f->failed_searches = link_failed_searches();
    f->count = (uint8_t)*count;
    return WS_LINK_READINGS_SIZE(*count);
}

static void apply_reply(const ws_link_reply_t *r)
{
    backlog_clear_dropped();

    if (r->unix_now >= CLOCK_VALID_FROM) {
        const uint32_t set_at = power_clock_set_at(), before = (uint32_t)time(NULL);
        const int64_t delta = power_set_clock(r->unix_now);
        backlog_shift(delta, set_at, before);   /* records still waiting move with the clock */
        if (delta > 2 || delta < -2) {
            ESP_LOGI(TAG, "clock set, moved by %lld s", delta);
        }
    }
    power_set_interval(r->interval_s);
    ota_note_reply(r);   /* an offered update runs after the exchange */
}

/* The backlog to the display. must_reach (a new firmware's first wake):
 * keep trying for PROBATION_MS until a reply comes. Returns whether one did. */
static bool exchange(battery_mode_t mode, bool must_reach)
{
    config_pairing_t pairing;
    if (!config_init() || !config_load_pairing(&pairing)) {
        ESP_LOGW(TAG, "not paired: flash the secrets image (radio stays off)");
        return false;
    }
    if (!link_start(&pairing, must_reach)) {
        link_stop();
        return false;
    }

    const int64_t until_us = must_reach ? esp_timer_get_time() + (int64_t)PROBATION_MS * 1000 : 0;
    uint16_t sent = 0, frames = 0;
    for (;;) {
        while (backlog_count() > 0) {
            uint8_t buf[WS_LINK_MAX_PAYLOAD];
            uint16_t count;
            const uint16_t seq = ++s_seq;
            const size_t len = build_frame(buf, seq, mode, &count);
            ws_link_reply_t reply = { 0 };
            const link_result_t res = link_transfer(buf, len, seq, &reply);
            if (res != LINK_NOT_HEARD) {
                power_frame_acked();   /* the display has this frame's counts */
            }
            if (res != LINK_REPLY) {
                break;
            }
            backlog_drop(count);
            apply_reply(&reply);
            sent += count;
            frames++;
        }
        if (frames > 0 || esp_timer_get_time() >= until_us) {
            break;
        }
        ESP_LOGW(TAG, "new firmware: no answer from the display yet, trying again");
        vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
    }
    link_stop();

    if (frames > 0) {
        ESP_LOGI(TAG, "sent %u records in %u frames on channel %u, rssi %d dBm, %u waiting",
                 sent, frames, link_channel(), link_last_rssi(), backlog_count());
    } else {
        ESP_LOGW(TAG, "display not reached, %u records waiting", backlog_count());
    }
    return frames > 0;
}

void app_main(void)
{
    /* ESP-IDF logs at WARN by default (sdkconfig.defaults); ours at INFO */
    static const char *const our_tags[] = {
        "ws_alpha", "power", "battery", "sensors", "backlog", "config", "link", "ota",
    };
    for (size_t i = 0; i < sizeof(our_tags) / sizeof(our_tags[0]); i++) {
        esp_log_level_set(our_tags[i], ESP_LOG_INFO);
    }

    power_begin();
    const bool probation = ota_on_probation();
    if (probation) {
        ESP_LOGW(TAG, "new firmware, first wake: it must reach the display within %d s", PROBATION_MS / 1000);
        power_extend_cap(PROBATION_CAP_MS);
    }

    const uint32_t now = (uint32_t)time(NULL);
    backlog_begin(now);

    ws_link_record_t rec = { .ts = now };
    rec.bat_mv = battery_read_mv();
    const battery_mode_t mode = battery_mode(rec.bat_mv);
    sensors_read(&rec);
    backlog_push(&rec);
    log_record(&rec, mode);

    if (probation) {
        /* the radio whatever the battery: without a reply it goes back */
        ota_end_probation(exchange(mode, true));
    } else if (radio_allowed(mode)) {
        exchange(mode, false);
        ota_run(rec.bat_mv);   /* returns unless an update restarts it */
    }
    power_sleep();
}

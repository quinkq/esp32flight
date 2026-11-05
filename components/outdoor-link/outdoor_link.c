/*
 * The display's side of the link to the outdoor unit: ws-link
 * (components/ws-link) over ESP-NOW, next to the display's normal Wi-Fi
 * connection.
 *
 *  - The outdoor unit is the only peer, encrypted with the shared PMK and
 *    LMK.
 *  - The radio stays on the access point's channel; the outdoor unit finds
 *    it by trying channels until a frame is acknowledged.
 *  - Wi-Fi power save is off: with it on, the radio sleeps between the
 *    access point's beacons and misses the outdoor unit's frames.
 *  - The receive callback runs in the Wi-Fi task and only queues the frame.
 *    A small task checks it, replies (clock, interval), logs it and puts its
 *    new readings into the store's RAM (components/readings-store).
 *  - The same task keeps the outdoor unit's health (outdoor_health.c) and
 *    does its housekeeping every 30 s, and about a second after the last
 *    frame of a burst: a catch-up sends its next frame right after our
 *    reply, so nothing slow runs in between. That's also when the store
 *    writes to flash, so the outdoor unit's 100 ms wait for a reply never
 *    depends on it (a crash in between loses that burst).
 */
#include "outdoor_link.h"

#include "esp_log.h"

static const char *TAG = "outdoor_link";

#if CONFIG_OUTDOOR_LINK_ENABLED

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "outdoor_health.h"
#include "readings_store.h"
#include "ws_link.h"

#define TASK_PRIORITY    5            /* above the app's tasks (3-4): the outdoor unit waits 100 ms */
#define TASK_STACK       6144         /* internal RAM; the daily summary logs how much is unused */
#define HOUSEKEEPING_MS  30000
#define BURST_MS         1000         /* quiet this long after a frame: the burst is over */
#define FUTURE_S         3600         /* a reading dated later than this past the display's time isn't stored */

typedef struct {
    uint8_t data[WS_LINK_MAX_PAYLOAD];
    uint8_t len;
    int8_t  rssi;
} rx_item_t;

static uint8_t s_peer[6];
static QueueHandle_t s_rx;

/* hex digits into n bytes; ':' and '-' separators are skipped */
static bool parse_hex(const char *s, uint8_t *out, size_t n)
{
    size_t got = 0;
    int hi = -1;
    for (; *s; s++) {
        if (*s == ':' || *s == '-') {
            continue;
        }
        if (!isxdigit((unsigned char)*s) || got == n) {
            return false;
        }
        const int v = isdigit((unsigned char)*s) ? *s - '0' : tolower((unsigned char)*s) - 'a' + 10;
        if (hi < 0) {
            hi = v;
        } else {
            out[got++] = (uint8_t)(hi << 4 | v);
            hi = -1;
        }
    }
    return got == n && hi < 0;
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len <= 0 || len > WS_LINK_MAX_PAYLOAD || memcmp(info->src_addr, s_peer, sizeof(s_peer)) != 0) {
        return;
    }
    rx_item_t item = {
        .len = (uint8_t)len,
        .rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0,
    };
    memcpy(item.data, data, len);
    xQueueSend(s_rx, &item, 0);
}

/* the MAC-level acknowledgement of our reply: the outdoor unit heard it */
static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    outdoor_health_reply_sent(status == ESP_NOW_SEND_SUCCESS);
}

static void reply(uint16_t seq)
{
    const time_t now = time(NULL);
    const ws_link_reply_t r = {
        .hdr = {
            .magic = WS_LINK_MAGIC,
            .version = WS_LINK_VERSION,
            .type = WS_LINK_REPLY,
            .seq = seq,
        },
        .unix_now = now >= CLOCK_VALID_FROM ? (uint32_t)now : 0,
        .interval_s = CONFIG_OUTDOOR_LINK_INTERVAL_S,
        .cmd = WS_CMD_NONE,
    };
    const esp_err_t err = esp_now_send(s_peer, (const uint8_t *)&r, WS_LINK_REPLY_SIZE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reply to seq %u not sent: %s", seq, esp_err_to_name(err));
    }
}

static void log_record(const ws_link_record_t *r)
{
    char sht[32] = "missing", bme[48] = "missing", bat[16] = "unknown";
    if (r->flags & WS_REC_SHT_OK) {
        snprintf(sht, sizeof(sht), "%.2f C %.2f %%", r->sht_t / 100.0, r->sht_rh / 100.0);
    }
    if (r->flags & WS_REC_BME_OK) {
        snprintf(bme, sizeof(bme), "%.2f C %.2f %% %.1f hPa", r->bme_t / 100.0, r->bme_rh / 100.0, r->bme_p / 10.0);
    }
    if (r->bat_mv != WS_LINK_NO_U16) {
        snprintf(bat, sizeof(bat), "%u mV", r->bat_mv);
    }
    ESP_LOGI(TAG, "  newest (ts %lu): SHT40 %s | BME280 %s | battery %s",
             (unsigned long)r->ts, sht, bme, bat);
}

/* The frame's new readings into the store. With the display's time they're
 * dated by ws_link_correct_ts(): the outdoor unit's clock runs at a slightly
 * wrong rate, so a reading's error grows with the time since its clock was
 * set (all of the offset after a power-on). now 0: the display has no time
 * yet, so the outdoor unit's clock was synced (the frame isn't answered
 * otherwise) and its times stay. The frame's RSSI goes with its newest
 * reading only. */
static void store(const ws_link_readings_t *f, const uint8_t *records, int8_t rssi,
                  const outdoor_marks_t *marks, time_t now)
{
    const int64_t offset = now != 0 ? (int64_t)now - f->outdoor_now : 0;
    for (unsigned i = 0; i < f->count; i++) {
        if (!(marks->fresh & 1u << i)) {
            continue;                   /* stored already: our reply to it got lost */
        }
        ws_link_record_t r;
        memcpy(&r, records + i * sizeof(r), sizeof(r));
        const uint32_t ts = ws_link_correct_ts(r.ts, offset, f->clock_set_at, f->outdoor_now);
        if (ts < CLOCK_VALID_FROM || (now != 0 && ts > now + FUTURE_S)) {
            ESP_LOGW(TAG, "  reading at %lu not stored: dated %lu", (unsigned long)r.ts, (unsigned long)ts);
            continue;
        }
        uint8_t flags = 0;
        if (marks->late & 1u << i) {
            flags |= READINGS_LATE;
        }
        if (f->flags & WS_OUTDOOR_LOW_BATTERY) {
            flags |= READINGS_LOW_BATTERY;
        }
        readings_store_put_outdoor(&r, ts, i + 1 == f->count ? rssi : READINGS_NO_RSSI, flags);
    }
}

static void handle(const rx_item_t *item)
{
    ws_link_readings_t f;   /* the fixed part; the records follow it */
    if (item->len < sizeof(f)) {
        ESP_LOGW(TAG, "%u-byte frame: too short", item->len);
        outdoor_health_rejected("a frame too short for ws-link");
        return;
    }
    memcpy(&f, item->data, sizeof(f));
    if (f.hdr.magic != WS_LINK_MAGIC || f.hdr.version != WS_LINK_VERSION || f.hdr.type != WS_LINK_READINGS) {
        ESP_LOGW(TAG, "not a ws-link v%d READINGS frame (magic 0x%04x, version %u, type %u)",
                 WS_LINK_VERSION, f.hdr.magic, f.hdr.version, f.hdr.type);
        char why[64];
        snprintf(why, sizeof(why), "not a ws-link v%d READINGS frame (version %u, type %u)",
                 WS_LINK_VERSION, f.hdr.version, f.hdr.type);
        outdoor_health_rejected(why);
        return;
    }
    if (f.count < 1 || f.count > WS_LINK_MAX_RECORDS || item->len != WS_LINK_READINGS_SIZE(f.count)) {
        ESP_LOGW(TAG, "seq %u: %u records in %u bytes don't fit", f.hdr.seq, f.count, item->len);
        outdoor_health_rejected("its readings don't fit the frame's size");
        return;
    }

    /* Without our own time, the records of an outdoor unit whose clock isn't
     * synced either can't be dated: no reply, so it keeps them and sends them
     * again next wake. Its counters restart on the acknowledgement all the
     * same, so the frame is logged either way. */
    const time_t now = time(NULL);
    const bool have_time = now >= CLOCK_VALID_FROM;
    const bool accept = have_time || (f.flags & WS_OUTDOOR_CLOCK_SYNCED);
    if (accept) {
        reply(f.hdr.seq);
    }

    char offset[48] = "unknown (no time yet)";
    if (have_time) {
        snprintf(offset, sizeof(offset), "%ld s", (long)(now - (time_t)f.outdoor_now));
        if (f.clock_set_at != 0) {
            snprintf(offset + strlen(offset), sizeof(offset) - strlen(offset), " (set %lu s ago)",
                     (unsigned long)(f.outdoor_now - f.clock_set_at));
        }
    }
    ESP_LOGI(TAG, "seq %u: %u records, fw %u, reset %u, flags 0x%02x, clock offset %s, rssi %d dBm",
             f.hdr.seq, f.count, f.fw_build, f.reset_reason, f.flags, offset, item->rssi);
    ESP_LOGI(TAG, "  since the last ack: awake %lu ms, brownouts %u, resets %u, failed searches %u",
             (unsigned long)f.awake_total_ms, f.brownouts, f.resets, f.failed_searches);
    if (!accept) {
        ESP_LOGW(TAG, "  no reply: no time yet and the outdoor unit's clock isn't synced, so these "
                      "records can't be dated; they come again next wake");
    }
    ws_link_record_t newest;
    memcpy(&newest, item->data + sizeof(f) + (f.count - 1) * sizeof(newest), sizeof(newest));
    log_record(&newest);

    outdoor_marks_t marks;
    outdoor_health_frame(&f, item->data + sizeof(f), item->rssi, accept, &marks);
    if (accept) {
        store(&f, item->data + sizeof(f), item->rssi, &marks, have_time ? now : 0);
    }
}

static void rx_task(void *arg)
{
    rx_item_t item;
    TickType_t wait = pdMS_TO_TICKS(HOUSEKEEPING_MS);
    for (;;) {
        if (xQueueReceive(s_rx, &item, wait) == pdTRUE) {
            handle(&item);
            wait = pdMS_TO_TICKS(BURST_MS);
            continue;
        }
        readings_store_flush();         /* first, so the tick sees how that went */
        outdoor_health_tick();
        wait = pdMS_TO_TICKS(HOUSEKEEPING_MS);
    }
}

void outdoor_link_start(void)
{
    uint8_t pmk[16], lmk[16];
    if (!parse_hex(CONFIG_OUTDOOR_LINK_PEER_MAC, s_peer, sizeof(s_peer)) ||
        !parse_hex(CONFIG_OUTDOOR_LINK_PMK, pmk, sizeof(pmk)) ||
        !parse_hex(CONFIG_OUTDOOR_LINK_LMK, lmk, sizeof(lmk))) {
        ESP_LOGE(TAG, "pairing incomplete or malformed in menuconfig; link off");
        return;
    }

    esp_now_peer_info_t peer = {
        .channel = 0,              /* whatever channel the access point uses */
        .ifidx = WIFI_IF_STA,
        .encrypt = true,
    };
    memcpy(peer.peer_addr, s_peer, sizeof(peer.peer_addr));
    memcpy(peer.lmk, lmk, sizeof(peer.lmk));

    s_rx = xQueueCreate(4, sizeof(rx_item_t));
    esp_err_t err = ESP_ERR_NO_MEM;
    if (!s_rx ||
        (err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK ||
        (err = esp_now_init()) != ESP_OK ||
        (err = esp_now_set_pmk(pmk)) != ESP_OK ||
        (err = esp_now_add_peer(&peer)) != ESP_OK ||
        (err = esp_now_register_recv_cb(on_recv)) != ESP_OK ||
        (err = esp_now_register_send_cb(on_sent)) != ESP_OK) {
        ESP_LOGE(TAG, "setup failed: %s; link off", esp_err_to_name(err));
        return;
    }
    if (!outdoor_health_init(CONFIG_OUTDOOR_LINK_INTERVAL_S) ||
        xTaskCreate(rx_task, "outdoor_link", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the task; link off");
        esp_now_deinit();
        return;
    }
    ESP_LOGI(TAG, "listening for the outdoor unit " MACSTR ", interval %d s", MAC2STR(s_peer),
             CONFIG_OUTDOOR_LINK_INTERVAL_S);
}

#else

void outdoor_link_start(void)
{
    ESP_LOGI(TAG, "disabled (no pairing in this build)");
}

#endif

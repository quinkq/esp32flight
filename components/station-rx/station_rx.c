/*
 * Receiver for the ws_alpha outdoor station: ws-link (components/ws-link)
 * over ESP-NOW, next to the display's normal Wi-Fi connection.
 *
 *  - The station is the only peer, encrypted with the shared PMK and LMK.
 *  - The radio stays on the access point's channel; the station finds it
 *    by trying channels until a frame is acknowledged.
 *  - Wi-Fi power save is off: with it on, the radio sleeps between the
 *    access point's beacons and misses the station's frames.
 *  - The receive callback runs in the Wi-Fi task and only queues the frame.
 *    A small task checks it, replies (clock, interval) and logs it. Storage
 *    comes later, and after the reply: records into RAM, the reply, then the
 *    flash write, so the station's 100 ms wait for the reply never depends
 *    on the file system (a crash in between loses that frame).
 */
#include "station_rx.h"

#include "esp_log.h"

static const char *TAG = "station_rx";

#if CONFIG_STATION_RX_ENABLED

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
#include "ws_link.h"

#define CLOCK_VALID_FROM 1767225600   /* 2026-01-01: earlier means no NTP time yet */
#define TASK_PRIORITY    5            /* above the app's tasks (3-4): the station waits 100 ms */

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
        .interval_s = CONFIG_STATION_RX_INTERVAL_S,
        .cmd = WS_CMD_NONE,
    };
    const esp_err_t err = esp_now_send(s_peer, (const uint8_t *)&r, WS_LINK_REPLY_SIZE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reply to seq %u not sent: %s", seq, esp_err_to_name(err));
    }
}

static void log_record(const ws_link_record_t *r)
{
    char sht[32] = "missing", bme[48] = "missing";
    if (r->flags & WS_REC_SHT_OK) {
        snprintf(sht, sizeof(sht), "%.2f C %.2f %%", r->sht_t / 100.0, r->sht_rh / 100.0);
    }
    if (r->flags & WS_REC_BME_OK) {
        snprintf(bme, sizeof(bme), "%.2f C %.2f %% %.1f hPa", r->bme_t / 100.0, r->bme_rh / 100.0, r->bme_p / 10.0);
    }
    ESP_LOGI(TAG, "  newest (ts %lu): SHT40 %s | BME280 %s | battery %u mV",
             (unsigned long)r->ts, sht, bme, r->bat_mv);
}

static void handle(const rx_item_t *item)
{
    ws_link_readings_t f;   /* the fixed part; the records follow it */
    if (item->len < sizeof(f)) {
        ESP_LOGW(TAG, "%u-byte frame: too short", item->len);
        return;
    }
    memcpy(&f, item->data, sizeof(f));
    if (f.hdr.magic != WS_LINK_MAGIC || f.hdr.version != WS_LINK_VERSION || f.hdr.type != WS_LINK_READINGS) {
        ESP_LOGW(TAG, "not a ws-link v%d READINGS frame (magic 0x%04x, version %u, type %u)",
                 WS_LINK_VERSION, f.hdr.magic, f.hdr.version, f.hdr.type);
        return;
    }
    if (f.count < 1 || f.count > WS_LINK_MAX_RECORDS || item->len != WS_LINK_READINGS_SIZE(f.count)) {
        ESP_LOGW(TAG, "seq %u: %u records in %u bytes don't fit", f.hdr.seq, f.count, item->len);
        return;
    }

    reply(f.hdr.seq);

    const long offset = (long)(time(NULL) - (time_t)f.station_now);
    ESP_LOGI(TAG, "seq %u: %u records, fw %u, reset %u, flags 0x%02x, clock offset %ld s, rssi %d dBm",
             f.hdr.seq, f.count, f.fw_build, f.reset_reason, f.flags, offset, item->rssi);
    ESP_LOGI(TAG, "  since the last ack: awake %lu ms, brownouts %u, resets %u, failed searches %u",
             (unsigned long)f.awake_total_ms, f.brownouts, f.resets, f.failed_searches);
    ws_link_record_t newest;
    memcpy(&newest, item->data + sizeof(f) + (f.count - 1) * sizeof(newest), sizeof(newest));
    log_record(&newest);
}

static void rx_task(void *arg)
{
    rx_item_t item;
    for (;;) {
        if (xQueueReceive(s_rx, &item, portMAX_DELAY) == pdTRUE) {
            handle(&item);
        }
    }
}

void station_rx_start(void)
{
    uint8_t pmk[16], lmk[16];
    if (!parse_hex(CONFIG_STATION_RX_PEER_MAC, s_peer, sizeof(s_peer)) ||
        !parse_hex(CONFIG_STATION_RX_PMK, pmk, sizeof(pmk)) ||
        !parse_hex(CONFIG_STATION_RX_LMK, lmk, sizeof(lmk))) {
        ESP_LOGE(TAG, "pairing incomplete or malformed in menuconfig; receiver off");
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
        (err = esp_now_register_recv_cb(on_recv)) != ESP_OK) {
        ESP_LOGE(TAG, "setup failed: %s; receiver off", esp_err_to_name(err));
        return;
    }
    if (xTaskCreate(rx_task, "station_rx", 4096, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no task; receiver off");
        esp_now_deinit();
        return;
    }
    ESP_LOGI(TAG, "listening for station " MACSTR ", interval %d s", MAC2STR(s_peer),
             CONFIG_STATION_RX_INTERVAL_S);
}

#else

void station_rx_start(void)
{
    ESP_LOGI(TAG, "disabled (no pairing in this build)");
}

#endif

/*
 * The radio side of ws-link (see PROTOCOL.md in the planning notes).
 *
 * The display's radio sits on its access point's channel, which the router
 * may change. The station has no SSID to scan for; it uses the MAC-level
 * acknowledgement of ESP-NOW instead: a frame the display heard is acked on
 * that channel. Order per wake: the cached channel (twice), then channels
 * 1-13. After 4 failed searches in a row, only every 4th wake searches, so
 * a display that is off for weeks costs little more than the cached try.
 *
 * Callbacks run in the Wi-Fi task; they only copy and signal.
 */
#include "link.h"

#include <string.h>
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "link";

#define CHANNEL_MAX          13        /* country PL */
#define SEND_TIMEOUT_MS      100       /* the send callback, acked or not */
#define REPLY_TIMEOUT_MS     100
#define SEARCH_FAILS_RATION  4         /* failed searches in a row before rationing */
#define SEARCH_EVERY         4         /* then search on every n-th wake */
#define STATE_MAGIC          0x57534c4bu

typedef struct {
    ws_link_reply_t reply;
    int8_t rssi;
} reply_item_t;

/* survives deep sleep and resets other than a power-on */
typedef struct {
    uint32_t magic;
    uint8_t  channel;          /* last channel the display acked on, 0 = unknown */
    uint8_t  failed_searches;
    uint8_t  skipped;          /* wakes without a search while rationed */
} link_state_t;

static RTC_NOINIT_ATTR link_state_t s_state;

static uint8_t s_peer[6];
static bool s_on_channel;      /* the display acked on the current channel this wake */
static int8_t s_last_rssi;
static SemaphoreHandle_t s_sent;
static volatile bool s_send_ok;
static QueueHandle_t s_replies;

static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    s_send_ok = status == ESP_NOW_SEND_SUCCESS;
    xSemaphoreGive(s_sent);
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (memcmp(info->src_addr, s_peer, sizeof(s_peer)) != 0 ||
        len < (int)WS_LINK_REPLY_SIZE || len > (int)sizeof(ws_link_reply_t)) {
        return;
    }
    reply_item_t item = { .rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0 };
    memcpy(&item.reply, data, len);
    if (item.reply.hdr.magic != WS_LINK_MAGIC || item.reply.hdr.version != WS_LINK_VERSION ||
        item.reply.hdr.type != WS_LINK_REPLY) {
        return;
    }
    xQueueSend(s_replies, &item, 0);
}

static void load_state(void)
{
    if (esp_reset_reason() == ESP_RST_POWERON || s_state.magic != STATE_MAGIC) {
        const uint8_t ch = config_load_channel();
        s_state = (link_state_t){
            .magic = STATE_MAGIC,
            .channel = ch <= CHANNEL_MAX ? ch : 0,
        };
    }
}

bool link_start(const config_pairing_t *pairing)
{
    load_state();
    memcpy(s_peer, pairing->peer_mac, sizeof(s_peer));
    s_on_channel = false;
    s_sent = xSemaphoreCreateBinary();
    s_replies = xQueueCreate(4, sizeof(reply_item_t));

    const wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_now_peer_info_t peer = {
        .channel = 0,              /* follow the radio's current channel */
        .ifidx = WIFI_IF_STA,
        .encrypt = true,
    };
    memcpy(peer.peer_addr, pairing->peer_mac, sizeof(peer.peer_addr));
    memcpy(peer.lmk, pairing->lmk, sizeof(peer.lmk));

    esp_err_t err = ESP_ERR_NO_MEM;
    if (!s_sent || !s_replies ||
        (err = esp_event_loop_create_default()) != ESP_OK ||
        (err = esp_wifi_init(&wifi_cfg)) != ESP_OK ||
        (err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||   /* no flash writes per wake */
        (err = esp_wifi_set_country_code("PL", false)) != ESP_OK ||    /* channels 1-13 */
        (err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK ||
        (err = esp_wifi_set_max_tx_power(CONFIG_WS_ALPHA_TX_POWER_DBM * 4)) != ESP_OK ||
        (err = esp_now_init()) != ESP_OK ||
        (err = esp_now_register_send_cb(on_sent)) != ESP_OK ||
        (err = esp_now_register_recv_cb(on_recv)) != ESP_OK ||
        (err = esp_now_set_pmk(pairing->pmk)) != ESP_OK ||
        (err = esp_now_add_peer(&peer)) != ESP_OK) {
        ESP_LOGE(TAG, "radio setup failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool set_channel(uint8_t ch)
{
    return esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) == ESP_OK;
}

/* one frame on the current channel; true if the display acked it */
static bool send_frame(const void *frame, size_t len)
{
    xSemaphoreTake(s_sent, 0);
    if (esp_now_send(s_peer, frame, len) != ESP_OK) {
        return false;
    }
    return xSemaphoreTake(s_sent, pdMS_TO_TICKS(SEND_TIMEOUT_MS)) == pdTRUE && s_send_ok;
}

static void found(uint8_t ch)
{
    s_on_channel = true;
    s_state.failed_searches = 0;
    s_state.skipped = 0;
    if (ch != s_state.channel) {
        ESP_LOGI(TAG, "display now on channel %u (was %u)", ch, s_state.channel);
        s_state.channel = ch;
        config_save_channel(ch);
    }
}

static bool search_allowed(void)
{
    if (s_state.failed_searches < SEARCH_FAILS_RATION) {
        return true;
    }
    if (++s_state.skipped >= SEARCH_EVERY) {
        s_state.skipped = 0;
        return true;
    }
    return false;
}

static bool find_display(const void *frame, size_t len)
{
    const uint8_t cached = s_state.channel;
    if (cached && set_channel(cached) && (send_frame(frame, len) || send_frame(frame, len))) {
        found(cached);
        return true;
    }
    if (!search_allowed()) {
        ESP_LOGW(TAG, "no ack on channel %u; search rationed (%u failed in a row)",
                 cached, s_state.failed_searches);
        return false;
    }
    const int64_t t0 = esp_timer_get_time();
    for (uint8_t ch = 1; ch <= CHANNEL_MAX; ch++) {
        if (ch != cached && set_channel(ch) && send_frame(frame, len)) {
            ESP_LOGI(TAG, "search: found on channel %u after %lld ms", ch, (esp_timer_get_time() - t0) / 1000);
            found(ch);
            return true;
        }
    }
    if (s_state.failed_searches < UINT8_MAX) {
        s_state.failed_searches++;
    }
    ESP_LOGW(TAG, "search: no ack on any channel (%lld ms, %u failed in a row)",
             (esp_timer_get_time() - t0) / 1000, s_state.failed_searches);
    return false;
}

bool link_transfer(const void *frame, size_t len, uint16_t seq, ws_link_reply_t *reply)
{
    xQueueReset(s_replies);
    const bool acked = s_on_channel ? send_frame(frame, len) : find_display(frame, len);
    if (!acked) {
        return false;
    }

    const int64_t deadline = esp_timer_get_time() + (int64_t)REPLY_TIMEOUT_MS * 1000;
    int64_t left;
    reply_item_t item;
    while ((left = deadline - esp_timer_get_time()) > 0 &&
           xQueueReceive(s_replies, &item, pdMS_TO_TICKS(left / 1000 + 1)) == pdTRUE) {
        if (item.reply.hdr.seq == seq) {
            memcpy(reply, &item.reply, sizeof(*reply));
            s_last_rssi = item.rssi;
            return true;
        }
    }
    ESP_LOGW(TAG, "acked but no reply to seq %u within %d ms", seq, REPLY_TIMEOUT_MS);
    return false;
}

uint8_t link_channel(void)
{
    return s_state.channel;
}

int8_t link_last_rssi(void)
{
    return s_last_rssi;
}

void link_stop(void)
{
    esp_now_deinit();
    esp_wifi_stop();
}

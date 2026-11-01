/*
 * Settings in NVS. The pairing (namespace "ws_link") comes from a secrets
 * image flashed once (secrets/flash-ws_alpha.ps1 in the workspace, outside
 * the repo), so neither the repo nor any firmware image carries a key. The
 * outdoor unit itself only writes its cached channel (namespace "ws_alpha"),
 * and only when it changes.
 */
#include "config.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";

#define NS_PAIRING "ws_link"
#define NS_OWN     "ws_alpha"

bool config_init(void)
{
    const esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS unusable (%s); not erasing it, it holds the pairing", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool read_blob(nvs_handle_t h, const char *key, void *out, size_t len)
{
    size_t n = len;
    return nvs_get_blob(h, key, out, &n) == ESP_OK && n == len;
}

bool config_load_pairing(config_pairing_t *p)
{
    nvs_handle_t h;
    if (nvs_open(NS_PAIRING, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    const bool ok = read_blob(h, "peer_mac", p->peer_mac, sizeof(p->peer_mac)) &&
                    read_blob(h, "pmk", p->pmk, sizeof(p->pmk)) &&
                    read_blob(h, "lmk", p->lmk, sizeof(p->lmk));
    nvs_close(h);
    return ok;
}

uint8_t config_load_channel(void)
{
    nvs_handle_t h;
    uint8_t channel = 0;
    if (nvs_open(NS_OWN, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "channel", &channel);
        nvs_close(h);
    }
    return channel;
}

void config_save_channel(uint8_t channel)
{
    nvs_handle_t h;
    if (nvs_open(NS_OWN, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_u8(h, "channel", channel) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "could not store channel %u", channel);
    }
    nvs_close(h);
}

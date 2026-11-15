/*
 * Settings in NVS. The pairing and the Wi-Fi networks (namespace "ws_link")
 * come from a secrets image flashed once (secrets/flash-ws_alpha.ps1 in the
 * workspace, outside the repo), so neither the repo nor any firmware image
 * carries a key. The outdoor unit itself writes only its cached channel and
 * its update attempts (namespace "ws_alpha"), each only when it changes.
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
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

bool config_load_wifi(int n, config_wifi_t *w)
{
    char key_ssid[12], key_pass[12];
    snprintf(key_ssid, sizeof(key_ssid), "wifi%d_ssid", n);
    snprintf(key_pass, sizeof(key_pass), "wifi%d_pass", n);
    memset(w, 0, sizeof(*w));
    nvs_handle_t h;
    if (nvs_open(NS_PAIRING, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t ns = sizeof(w->ssid), np = sizeof(w->pass);
    const bool ok = nvs_get_str(h, key_ssid, w->ssid, &ns) == ESP_OK && w->ssid[0] != '\0' &&
                    nvs_get_str(h, key_pass, w->pass, &np) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool config_load_ota(config_ota_t *o)
{
    memset(o, 0, sizeof(*o));
    nvs_handle_t h;
    if (nvs_open(NS_OWN, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    const bool ok = read_blob(h, "ota", o, sizeof(*o));
    nvs_close(h);
    if (!ok) {
        memset(o, 0, sizeof(*o));
    }
    return ok;
}

void config_save_ota(const config_ota_t *o)
{
    nvs_handle_t h;
    if (nvs_open(NS_OWN, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, "ota", o, sizeof(*o)) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "could not store the update attempts");
    }
    nvs_close(h);
}

void config_erase_ota(void)
{
    nvs_handle_t h;
    if (nvs_open(NS_OWN, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_erase_key(h, "ota") == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

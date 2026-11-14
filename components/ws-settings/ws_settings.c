/*
 * The weather station's own settings (ws_settings.h; "Settings" in
 * DISPLAY.md, in the planning notes).
 *
 *  - One NVS key per field, in the namespace "ws", so fields can be added
 *    later without losing the saved ones; a missing key keeps its default.
 *  - Defaults: the pairing and the interval from menuconfig (the secrets
 *    fragment), everything else neutral.
 *  - A mutex around the copy in RAM, held through a save, so two saves
 *    can't interleave.
 */
#include "ws_settings.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "ws_settings";

#define NAMESPACE "ws"

typedef enum { K_BOOL, K_U16, K_I16, K_I32, K_BLOB } kind_t;

#define FIELD(key, member, kind) \
    { key, offsetof(ws_settings_t, member), sizeof(((ws_settings_t *)0)->member), kind }

static const struct {
    const char *key;                    /* at most 15 characters (NVS) */
    size_t off, len;
    kind_t kind;
} FIELDS[] = {
    FIELD("link_on", link_on, K_BOOL),
    FIELD("mac", mac, K_BLOB),
    FIELD("pmk", pmk, K_BLOB),
    FIELD("lmk", lmk, K_BLOB),
    FIELD("interval", interval_s, K_U16),
    FIELD("indoor_on", indoor_on, K_BOOL),
    FIELD("cal_out_t", cal_out_t, K_I16),
    FIELD("cal_out_rh", cal_out_rh, K_I16),
    FIELD("cal_in_t", cal_in_t, K_I16),
    FIELD("cal_in_rh", cal_in_rh, K_I16),
    FIELD("bat_trim", bat_trim_pm, K_I16),
    FIELD("alt_override", alt_override_m, K_I16),
    FIELD("alt_known", alt_known, K_BOOL),
    FIELD("alt_m", alt_m, K_I16),
    FIELD("alt_lat", alt_lat_e5, K_I32),
    FIELD("alt_lon", alt_lon_e5, K_I32),
};

static SemaphoreHandle_t s_mux;
static ws_settings_t s_cur;

bool ws_settings_parse_hex(const char *s, uint8_t *out, size_t n)
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

static bool all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

bool ws_settings_paired(const ws_settings_t *s)
{
    return !all_zero(s->mac, sizeof(s->mac)) && !all_zero(s->pmk, sizeof(s->pmk)) &&
           !all_zero(s->lmk, sizeof(s->lmk));
}

static void defaults(ws_settings_t *s)
{
    *s = (ws_settings_t){
        .link_on = true,
        .interval_s = 900,
        .indoor_on = true,
        .alt_override_m = WS_ALT_AUTO,
    };
#if CONFIG_OUTDOOR_LINK_ENABLED
    s->interval_s = CONFIG_OUTDOOR_LINK_INTERVAL_S;
    if (!ws_settings_parse_hex(CONFIG_OUTDOOR_LINK_PEER_MAC, s->mac, sizeof(s->mac)) ||
        !ws_settings_parse_hex(CONFIG_OUTDOOR_LINK_PMK, s->pmk, sizeof(s->pmk)) ||
        !ws_settings_parse_hex(CONFIG_OUTDOOR_LINK_LMK, s->lmk, sizeof(s->lmk))) {
        memset(s->mac, 0, sizeof(s->mac));
        memset(s->pmk, 0, sizeof(s->pmk));
        memset(s->lmk, 0, sizeof(s->lmk));
    }
#endif
}

static void load(nvs_handle_t h, ws_settings_t *s)
{
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
        uint8_t *p = (uint8_t *)s + FIELDS[i].off;
        const char *key = FIELDS[i].key;
        switch (FIELDS[i].kind) {
        case K_BOOL: {
            uint8_t v;
            if (nvs_get_u8(h, key, &v) == ESP_OK) {
                *(bool *)p = v != 0;
            }
            break;
        }
        case K_U16: {
            uint16_t v;
            if (nvs_get_u16(h, key, &v) == ESP_OK) {
                *(uint16_t *)p = v;
            }
            break;
        }
        case K_I16: {
            int16_t v;
            if (nvs_get_i16(h, key, &v) == ESP_OK) {
                *(int16_t *)p = v;
            }
            break;
        }
        case K_I32: {
            int32_t v;
            if (nvs_get_i32(h, key, &v) == ESP_OK) {
                *(int32_t *)p = v;
            }
            break;
        }
        case K_BLOB: {
            uint8_t v[16];
            size_t len = sizeof(v);
            if (nvs_get_blob(h, key, v, &len) == ESP_OK && len == FIELDS[i].len) {
                memcpy(p, v, len);
            }
            break;
        }
        }
    }
}

static esp_err_t save(const ws_settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; err == ESP_OK && i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
        const uint8_t *p = (const uint8_t *)s + FIELDS[i].off;
        const char *key = FIELDS[i].key;
        switch (FIELDS[i].kind) {
        case K_BOOL:
            err = nvs_set_u8(h, key, *(const bool *)p ? 1 : 0);
            break;
        case K_U16:
            err = nvs_set_u16(h, key, *(const uint16_t *)p);
            break;
        case K_I16:
            err = nvs_set_i16(h, key, *(const int16_t *)p);
            break;
        case K_I32:
            err = nvs_set_i32(h, key, *(const int32_t *)p);
            break;
        case K_BLOB:
            err = nvs_set_blob(h, key, p, FIELDS[i].len);
            break;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

void ws_settings_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    defaults(&s_cur);
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        load(h, &s_cur);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "link %s, %s, interval %u s, indoor sensor %s", s_cur.link_on ? "on" : "off",
             ws_settings_paired(&s_cur) ? "paired" : "not paired", s_cur.interval_s,
             s_cur.indoor_on ? "on" : "off");
}

void ws_settings_get(ws_settings_t *out)
{
    if (s_mux == NULL) {
        defaults(out);
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_cur;
    xSemaphoreGive(s_mux);
}

static const char *check(const ws_settings_t *s, const ws_settings_t *cur)
{
    /* an interval outside the range stays as it is (the bench's 1 minute from menuconfig) */
    if (s->interval_s != cur->interval_s &&
        (s->interval_s < WS_INTERVAL_MIN_S || s->interval_s > WS_INTERVAL_MAX_S)) {
        return "the interval must be 3 minutes to 6 hours";
    }
    if (abs(s->cal_out_t) > 500 || abs(s->cal_in_t) > 500) {
        return "temperature offsets: 5 degrees at most";
    }
    if (abs(s->cal_out_rh) > 1000 || abs(s->cal_in_rh) > 1000) {
        return "humidity offsets: 10 % at most";
    }
    if (abs(s->bat_trim_pm) > 50) {
        return "battery factor: 50 per mille at most";
    }
    if (s->alt_override_m != WS_ALT_AUTO && (s->alt_override_m < -100 || s->alt_override_m > 3000)) {
        return "altitude: -100 to 3000 m";
    }
    return NULL;
}

esp_err_t ws_settings_set(const ws_settings_t *in, bool *restart, const char **why)
{
    *restart = false;
    *why = NULL;
    if (s_mux == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    ws_settings_t next = *in;
    next.alt_known = s_cur.alt_known;
    next.alt_m = s_cur.alt_m;
    next.alt_lat_e5 = s_cur.alt_lat_e5;
    next.alt_lon_e5 = s_cur.alt_lon_e5;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if ((*why = check(&next, &s_cur)) == NULL) {
        *restart = next.link_on != s_cur.link_on || memcmp(next.mac, s_cur.mac, sizeof(next.mac)) != 0 ||
                   memcmp(next.pmk, s_cur.pmk, sizeof(next.pmk)) != 0 ||
                   memcmp(next.lmk, s_cur.lmk, sizeof(next.lmk)) != 0;
        if ((err = save(&next)) == ESP_OK) {
            s_cur = next;
        } else {
            *why = "couldn't save to NVS";
            *restart = false;
        }
    }
    xSemaphoreGive(s_mux);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved: link %s, interval %u s, indoor sensor %s%s", next.link_on ? "on" : "off",
                 next.interval_s, next.indoor_on ? "on" : "off", *restart ? "; pairing changed" : "");
    }
    return err;
}

esp_err_t ws_settings_set_altitude(int16_t m, int32_t lat_e5, int32_t lon_e5)
{
    if (s_mux == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    ws_settings_t next = s_cur;
    next.alt_known = true;
    next.alt_m = m;
    next.alt_lat_e5 = lat_e5;
    next.alt_lon_e5 = lon_e5;
    const esp_err_t err = save(&next);
    if (err == ESP_OK) {
        s_cur = next;
    }
    xSemaphoreGive(s_mux);
    return err;
}

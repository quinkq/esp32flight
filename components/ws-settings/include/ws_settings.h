#pragma once

/*
 * The weather station's own settings (weather-station fork): the outdoor
 * unit's pairing and interval, the indoor sensor, the calibration. Kept in
 * their own NVS namespace, apart from upstream's settings. Until something
 * is saved, the pairing and the interval come from menuconfig ("Outdoor unit
 * link", filled by the git-ignored secrets fragment).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define WS_INTERVAL_MIN_S 180            /* what can be set: 3 min ... */
#define WS_INTERVAL_MAX_S 21600          /* ... to 6 h */
#define WS_ALT_AUTO       INT16_MIN      /* alt_override_m: from the home location */

typedef struct {
    bool     link_on;                    /* the link to the outdoor unit */
    uint8_t  mac[6];                     /* the outdoor unit's; all zero: not paired */
    uint8_t  pmk[16], lmk[16];           /* the ESP-NOW keys; never shown back */
    uint16_t interval_s;                 /* the outdoor unit's measurement interval */
    bool     indoor_on;                  /* the indoor SHT40 */
    int16_t  cal_out_t, cal_out_rh;      /* added to the outdoor SHT40's values, hundredths */
    int16_t  cal_in_t, cal_in_rh;        /* added to the indoor sensor's */
    int16_t  bat_trim_pm;                /* the outdoor unit's battery voltage × (1000 + this) / 1000 */
    int16_t  alt_override_m;             /* WS_ALT_AUTO: from the home location */
    /* the altitude looked up for the home location (altitude.c), cached */
    bool     alt_known;
    int16_t  alt_m;
    int32_t  alt_lat_e5, alt_lon_e5;     /* the coordinates it is for, degrees × 100000 */
} ws_settings_t;

/* Once at boot, after nvs_flash_init(). */
void ws_settings_init(void);

/* A copy of the current settings. */
void ws_settings_get(ws_settings_t *out);

/* Checks and saves new settings; the cached altitude in `in` is ignored.
 * *restart: the link's switch or its pairing changed, which takes effect
 * only after a restart. ESP_ERR_INVALID_ARG with *why set when a value is out
 * of range; nothing is saved then. */
esp_err_t ws_settings_set(const ws_settings_t *in, bool *restart, const char **why);

/* The altitude looked up for these coordinates (degrees × 100000). */
esp_err_t ws_settings_set_altitude(int16_t m, int32_t lat_e5, int32_t lon_e5);

/* The outdoor unit's MAC and both keys are set. */
bool ws_settings_paired(const ws_settings_t *s);

/* Hex digits into exactly n bytes; ':' and '-' between them are skipped. */
bool ws_settings_parse_hex(const char *s, uint8_t *out, size_t n);

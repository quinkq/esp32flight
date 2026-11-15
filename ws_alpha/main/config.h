#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t peer_mac[6];       /* the display's Wi-Fi MAC (client interface) */
    uint8_t pmk[16];
    uint8_t lmk[16];
} config_pairing_t;

/* NVS up. Never erases on errors: the partition holds the pairing. */
bool config_init(void);

/* The pairing from the secrets image; false if it's missing or incomplete. */
bool config_load_pairing(config_pairing_t *p);

/* The last channel the display answered on, 0 if none is stored. */
uint8_t config_load_channel(void);
void config_save_channel(uint8_t channel);

/* Wi-Fi network n from the secrets image (1: the user's, 2: the parents'),
 * used for updates only; false if it isn't stored. */
typedef struct {
    char ssid[33];
    char pass[65];
} config_wifi_t;
bool config_load_wifi(int n, config_wifi_t *w);

/* Update attempts (ota.c), in NVS so they survive anything, firmware
 * changes too: keep this layout in every build. */
typedef struct {
    uint8_t  sha8[8];          /* the first 8 bytes of the image's SHA-256 */
    uint8_t  tries;            /* attempts at that image */
    uint8_t  reserved[3];
    uint32_t last_try;         /* the clock at the last attempt, any image; 0: none */
} config_ota_t;
bool config_load_ota(config_ota_t *o);   /* false: none stored (o zeroed) */
void config_save_ota(const config_ota_t *o);
void config_erase_ota(void);

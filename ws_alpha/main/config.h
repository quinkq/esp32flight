#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t peer_mac[6];       /* the display's Wi-Fi station MAC */
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

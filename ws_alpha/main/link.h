#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "ws_link.h"

/* Wi-Fi (station mode, never connected) and ESP-NOW with the display as
 * the encrypted peer, on the cached channel. */
bool link_start(const config_pairing_t *pairing);

/* Sends one READINGS frame and waits for the display's REPLY with the same
 * seq. The first transfer of a wake finds the display (cached channel, one
 * retry, then a search over channels 1-13, rationed after repeated
 * failures). True with *reply filled if the display answered. */
bool link_transfer(const void *frame, size_t len, uint16_t seq, ws_link_reply_t *reply);

/* The channel in use, and the signal strength of the last reply (dBm, 0 if
 * none came): for the log, and for tuning the transmit power. */
uint8_t link_channel(void);
int8_t link_last_rssi(void);

/* Radio off, before deep sleep. */
void link_stop(void);

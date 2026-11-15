#pragma once

/* The update side of outdoor-link (outdoor_ota.c), as outdoor_link.c uses
 * it; the upload and download side is in include/outdoor_link.h. */

#include <stdbool.h>
#include <stdint.h>
#include "ws_link.h"

/* Once, from outdoor_link_start(). Without it every upload is refused. */
bool outdoor_ota_init(void);

/* A valid frame, before the reply to it: the build it runs, whether it gave
 * up on the offered image (WS_OUTDOOR_OTA_FAILED) and its reset reason. */
void outdoor_ota_frame(uint16_t fw_build, bool gave_up, uint8_t reset_reason);

/* Fills the offer into a reply while an image waits; true: send it as
 * WS_LINK_REPLY_OTA_SIZE. */
bool outdoor_ota_offer(ws_link_reply_t *r);

/* From the ESP-NOW send callback (the Wi-Fi task): the outdoor unit's radio
 * acknowledged a reply that carried the offer. */
void outdoor_ota_offer_heard(void);

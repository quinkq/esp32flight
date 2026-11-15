#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ws_link.h"

/* The first wake of a new firmware, still on probation: it must reach the
 * display in this wake (ota_end_probation()). */
bool ota_on_probation(void);

/* Got a reply from the display (passed) or not (gone back to the previous
 * firmware: a restart, so this returns only when there's none to go back
 * to). Needs NVS up. */
void ota_end_probation(bool replied);

/* Every reply from the display: notes an offer (WS_CMD_OTA) for after the
 * exchange, and forgets the attempts at an image no longer offered. Needs
 * NVS up. */
void ota_note_reply(const ws_link_reply_t *r);

/* For the frames: gave up on the offered image (WS_OUTDOOR_OTA_FAILED).
 * Needs NVS up. */
bool ota_gave_up(void);

/* After the exchange, with the radio stopped: the offered update, if the
 * battery and the attempt rules allow it. Restarts into the new firmware
 * when it worked; returns otherwise. */
void ota_run(uint16_t bat_mv);

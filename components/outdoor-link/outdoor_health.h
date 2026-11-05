#pragma once

/* The health side of outdoor-link (outdoor_health.c), fed by outdoor_link.c
 * from its task, except outdoor_health_reply_sent(). */

#include <stdbool.h>
#include <stdint.h>
#include "ws_link.h"

#define CLOCK_VALID_FROM 1767225600   /* 2026-01-01: earlier means no NTP time yet */

bool outdoor_health_init(uint32_t interval_s);

/* What outdoor_health_frame() found, bit i for the frame's reading i: fresh =
 * not seen before (the outdoor unit sends readings again when our reply got
 * lost), late = from its backlog. */
typedef struct {
    uint16_t fresh, late;
} outdoor_marks_t;

_Static_assert(WS_LINK_MAX_RECORDS <= 16, "a bit per reading in outdoor_marks_t");

/* A valid READINGS frame; records: the f->count records that follow it in
 * the payload. answered: the display replied (it doesn't when the frame
 * can't be dated; its readings then come again with the next wake, and none
 * is marked). */
void outdoor_health_frame(const ws_link_readings_t *f, const uint8_t *records, int8_t rssi,
                          bool answered, outdoor_marks_t *marks);

/* A frame from the paired outdoor unit that isn't usable ws-link. */
void outdoor_health_rejected(const char *why);

/* From the ESP-NOW send callback, in the Wi-Fi task. */
void outdoor_health_reply_sent(bool acked);

/* Housekeeping: status changes, the display's channel, local midnight, the
 * readings store's file, pending events into the log file. */
void outdoor_health_tick(void);

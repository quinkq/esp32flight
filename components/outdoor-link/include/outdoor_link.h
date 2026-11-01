#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The display's side of the link to the outdoor unit (ws-link over
 * ESP-NOW). Call once after Wi-Fi has started. Does nothing unless it's
 * enabled and paired in menuconfig ("Outdoor unit link"). */
void outdoor_link_start(void);

typedef enum {
    OUTDOOR_NOT_HEARD,                 /* no frame since the display started */
    OUTDOOR_OK,
    OUTDOOR_LATE,                      /* no frame for 1.5 expected gaps */
    OUTDOOR_SILENT,                    /* no frame for 3 expected gaps */
} outdoor_status_t;

/* The outdoor unit as the display sees it. */
typedef struct {
    bool     running;                  /* paired and listening */
    outdoor_status_t status;
    int64_t  last_heard_us;            /* esp_timer_get_time() of the last frame; 0: none */
    uint32_t expected_gap_s;           /* the interval, x4 in low-battery mode */
    uint32_t uptime_s;                 /* the display's: what the counters below cover */

    /* the latest frame */
    uint16_t fw_build;
    uint8_t  reset_reason;             /* esp_reset_reason_t of its current boot */
    bool     clock_synced, low_battery, backlog_dropped;
    bool     sht_ok, bme_ok;           /* in its newest reading */
    uint16_t bat_mv;                   /* its newest reading; 0xFFFF: unknown */
    int8_t   rssi;                     /* dBm, as the display received the frame */
    uint8_t  records;                  /* readings in the frame */
    bool     offset_known;             /* false while the display had no time */
    int64_t  clock_offset_s;           /* display clock minus the outdoor unit's */
    uint32_t awake_total_ms;           /* since its previous acknowledged frame, as are: */
    uint8_t  brownouts, resets, failed_searches;

    /* counted by the display since it started */
    uint32_t frames, rejected, unanswered, replies_unacked;
    uint32_t readings, twice, late, missing;
    uint8_t  channel;                  /* the display's Wi-Fi channel; 0: not connected yet */
    uint16_t channel_changes;
} outdoor_health_t;

/* A copy of the current state, with the status worked out at the call. */
void outdoor_link_health(outdoor_health_t *out);

typedef struct {
    int64_t unix_time;                 /* 0 while the display has no time */
    char    kind[12];                  /* "first", "silent", "back", "day", ... */
    char    text[256];
} outdoor_event_t;

/* Up to max of the latest events, newest first; returns how many. */
size_t outdoor_link_events(outdoor_event_t *out, size_t max);

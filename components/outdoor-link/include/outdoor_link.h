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
    int32_t  assets_free_kb;           /* free in the /assets partition; -1: unknown */
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

/* An event from elsewhere in the weather station (the indoor sensor) into
 * the same log. Any task; serial log only while the link isn't running. */
void outdoor_link_event(const char *kind, const char *text);

/* A firmware image for the outdoor unit, uploaded on the weather station
 * page and offered in the replies until the outdoor unit runs it
 * (outdoor_ota.c). Kept in the display's spare app slot: it survives a
 * display restart, a display update replaces it. */
typedef enum {
    OUTDOOR_OTA_NONE,                  /* nothing uploaded since the display started */
    OUTDOOR_OTA_WAITING,               /* offered in every reply */
    OUTDOOR_OTA_STARTED,               /* a frame from the new build came: not offered any more */
    OUTDOOR_OTA_DONE,                  /* ... and one after a deep-sleep wake on it: it kept it */
    OUTDOOR_OTA_FAILED,                /* see why */
    OUTDOOR_OTA_CANCELLED,
} outdoor_ota_state_t;

typedef struct {
    outdoor_ota_state_t state;
    int64_t  changed_us;               /* esp_timer_get_time() of the last state change */
    uint16_t build;                    /* the image's build number */
    uint16_t from_build;               /* the outdoor unit's when it was uploaded; 0: not heard yet */
    uint32_t size;
    uint16_t offers_heard;             /* replies with the offer its radio acknowledged */
    uint16_t downloads, downloads_complete;
    char     why[160];                 /* FAILED: why */
} outdoor_ota_info_t;

/* The upload, from the web server: begin with the size, write the pieces
 * in order, end. Each returns NULL to go on, or why not (then it's over).
 * end() checks the image (ESP32-C3, ws_alpha, signed, a new build number)
 * and offers it from the next frame on; a new upload replaces a waiting one.
 * Refused while the display itself is on probation after its own update:
 * the spare slot is its way back then. */
const char *outdoor_ota_begin(size_t size);
const char *outdoor_ota_write(const void *data, size_t len);
const char *outdoor_ota_end(void);
void outdoor_ota_abort(void);          /* an upload that broke off */

/* Stop offering a waiting image; false if none waited. */
bool outdoor_ota_cancel(void);

void outdoor_ota_info(outdoor_ota_info_t *out);

/* The outdoor unit's download, from the web server: begin (false unless an
 * image waits), read pieces by offset, end (complete or not, bytes sent). */
bool outdoor_ota_download_begin(uint32_t *size);
size_t outdoor_ota_download_read(uint32_t offset, void *buf, size_t len);
void outdoor_ota_download_end(bool complete, uint32_t sent);

/* The event log, one event per line: unix time, kind, text, tab-separated.
 * Past 16 KB the file becomes the .old one, so read both, old first. */
#define OUTDOOR_LOG_PATH     "/assets/outdoor.tsv"
#define OUTDOOR_LOG_OLD_PATH "/assets/outdoor.old.tsv"

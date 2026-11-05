#pragma once

/*
 * The weather station's readings, one record per 15-minute slot, ~23 days:
 * all of them in PSRAM, with a copy in the "readings" flash partition (the
 * unused 56 KB before the first app slot, partitions.csv).
 *
 * Readings go into RAM; readings_store_flush() writes what changed to flash.
 * outdoor-link calls it once a burst of frames is over, so its replies never
 * wait for the flash.
 */

#include <stdbool.h>
#include <stdint.h>
#include "ws_link.h"

#define READINGS_SLOT_S      900                    /* the grid: 15 minutes */
#define READINGS_SLOTS       (13 * 170)             /* 13 flash sectors of 170 slots: ~23 days */
#define READINGS_PARTITION   "readings"

/* readings_slot_t.flags */
#define READINGS_SHT_OK      (1u << 0)              /* sht_* hold a mean (the same bit as WS_REC_SHT_OK) */
#define READINGS_BME_OK      (1u << 1)              /* bme_* hold a mean (WS_REC_BME_OK) */
#define READINGS_INDOOR_OK   (1u << 2)              /* in_* hold a mean */
#define READINGS_LATE        (1u << 3)              /* a reading came from the outdoor unit's backlog */
#define READINGS_LOW_BATTERY (1u << 4)              /* the outdoor unit was in low-battery mode */

#define READINGS_NO_RSSI     INT8_MIN

/* One slot as stored: raw values (calibration is applied when reading), each
 * the mean of the slot's readings that have it. Missing values as in ws-link
 * (WS_LINK_NO_I16, WS_LINK_NO_U16), with their flag cleared. */
typedef struct {
    uint32_t ts;               /* the slot's start, Unix seconds; 0: empty */
    int16_t  sht_t;            /* SHT40, 0.01 °C */
    uint16_t sht_rh;           /* SHT40, 0.01 %RH */
    int16_t  bme_t;            /* BME280, 0.01 °C */
    uint16_t bme_rh;           /* BME280, 0.01 %RH */
    uint16_t bme_p;            /* BME280, 0.1 hPa */
    uint16_t bat_mv;           /* the outdoor unit's battery */
    int8_t   rssi;             /* dBm of the frame whose newest reading is in this slot; READINGS_NO_RSSI: none */
    uint8_t  flags;            /* READINGS_* */
    int16_t  in_t;             /* indoor SHT40, 0.01 °C */
    uint16_t in_rh;            /* indoor SHT40, 0.01 %RH */
    uint8_t  count;            /* outdoor readings in the means */
    uint8_t  check;            /* CRC-8 of the bytes before it */
} readings_slot_t;

/* Once at boot: reads the partition into RAM. Without the partition (its
 * partition table not flashed) the readings stay in RAM. */
void readings_store_init(void);

/* A reading from the outdoor unit, ts already on the display's time base. It
 * goes to the slot whose start is nearest. rssi: READINGS_NO_RSSI unless it's
 * the frame's newest reading. flags: READINGS_LATE, READINGS_LOW_BATTERY. Call
 * each reading once only: repeats would count twice in the means. */
void readings_store_put_outdoor(const ws_link_record_t *r, uint32_t ts, int8_t rssi, uint8_t flags);

/* What changed in RAM into flash: about 60 ms per sector written. Never call
 * it while someone waits for a quick answer. */
void readings_store_flush(void);

/* A copy of the newest slot; false while there's none. */
bool readings_store_latest(readings_slot_t *out);

/* What a client has seen, as a token for URLs: "<boot>-<change>". Every
 * change to a slot gets the next change number; a token from an earlier
 * boot counts as having seen nothing. */
#define READINGS_CURSOR_LEN  24

/* The token for everything up to now. Take it before readings_store_changed():
 * a slot changed in between is sent again next time, never missed. */
void readings_store_cursor(char *out);

typedef bool (*readings_fn)(const readings_slot_t *s, void *ctx);

/* The slots changed since the token `after` (NULL or "": all) with ts > since,
 * oldest first, to fn, which may block: it's called outside the store's
 * lock. Stops early, returning false, when fn returns false. */
bool readings_store_changed(const char *after, uint32_t since, readings_fn fn, void *ctx);

typedef enum {
    READINGS_OFF,              /* not started, or no memory */
    READINGS_FLASH,            /* readings kept in flash too */
    READINGS_RAM_ONLY,         /* no flash (see why): lost at a restart */
} readings_store_state_t;

typedef struct {
    readings_store_state_t state;
    char     why[112];         /* READINGS_RAM_ONLY: why not in flash */
    bool     write_failing;    /* the last flash write failed; the readings wait in RAM */
    uint32_t slots;            /* filled slots */
    uint32_t oldest, newest;   /* their ts; 0: none */
    uint32_t unwritten;        /* slots changed since flash last got them */
} readings_store_info_t;

void readings_store_info(readings_store_info_t *out);

/*
 * ws-link: the radio protocol between the outdoor unit (ESP32-C3, firmware
 * ws_alpha) and the display (the 7B). Both firmwares include this header,
 * so the two sides can't drift apart.
 *
 *  - ESP-NOW unicast between two configured peers, encrypted (PMK + LMK).
 *    The encryption also authenticates every frame, so there's no CRC.
 *  - Payload at most 250 bytes (ESP-NOW v1), little-endian, packed structs,
 *    integers only.
 *  - The outdoor unit sends READINGS (its backlog, oldest first); the display
 *    takes the records into RAM, answers with a REPLY echoing the seq, then
 *    writes them to flash.
 *
 * Packed members can be unaligned: build frames in a buffer and copy whole
 * structs with memcpy() rather than taking pointers to single fields.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define WS_LINK_MAGIC              0x5357  /* bytes 'W','S' on the wire */
#define WS_LINK_VERSION            4

#define WS_LINK_MAX_PAYLOAD        250     /* ESP-NOW v1 */
#define WS_LINK_MAX_RECORDS        13      /* per READINGS frame */
#define WS_LINK_DEFAULT_INTERVAL_S 900     /* the display may set 60 s ... 6 h */

/* ws_link_hdr_t.type */
enum {
    WS_LINK_READINGS = 1,                  /* outdoor unit -> display */
    WS_LINK_REPLY    = 2,                  /* display -> outdoor unit */
};

/* ws_link_record_t.flags: a value is valid only with its flag set */
#define WS_REC_SHT_OK              (1u << 0)
#define WS_REC_BME_OK              (1u << 1)

/* missing values, with the matching WS_REC_* flag cleared */
#define WS_LINK_NO_I16             INT16_MIN
#define WS_LINK_NO_U16             UINT16_MAX

/* ws_link_readings_t.flags */
#define WS_OUTDOOR_CLOCK_SYNCED    (1u << 0)  /* clock set by the display since power-on */
#define WS_OUTDOOR_BACKLOG_DROPPED (1u << 1)  /* oldest unsent records were discarded */
#define WS_OUTDOOR_LOW_BATTERY     (1u << 2)  /* reduced mode: radio every 4th wake */
#define WS_OUTDOOR_OTA_FAILED      (1u << 3)  /* the last OTA attempt failed */

/* ws_link_reply_t.cmd */
enum {
    WS_CMD_NONE = 0,
    WS_CMD_OTA  = 1,
};

typedef struct __attribute__((packed)) {
    uint16_t magic;            /* WS_LINK_MAGIC */
    uint8_t  version;          /* WS_LINK_VERSION */
    uint8_t  type;             /* WS_LINK_READINGS or WS_LINK_REPLY */
    uint16_t seq;              /* per-frame counter; the reply echoes it */
} ws_link_hdr_t;

/* one measurement */
typedef struct __attribute__((packed)) {
    uint32_t ts;               /* outdoor unit's clock at measurement, Unix seconds */
    int16_t  sht_t;            /* SHT40 temperature, 0.01 °C */
    uint16_t sht_rh;           /* SHT40 humidity, 0.01 %RH */
    int16_t  bme_t;            /* BME280 temperature, 0.01 °C */
    uint16_t bme_rh;           /* BME280 humidity, 0.01 %RH */
    uint16_t bme_p;            /* BME280 pressure, 0.1 hPa */
    uint16_t bat_mv;           /* battery voltage before the radio starts, mV */
    uint8_t  flags;            /* WS_REC_* */
} ws_link_record_t;

/* outdoor unit -> display */
typedef struct __attribute__((packed)) {
    ws_link_hdr_t hdr;         /* type = WS_LINK_READINGS */
    uint32_t outdoor_now;      /* outdoor unit's clock when the frame was sent */
    uint32_t clock_set_at;     /* its clock when a reply last set it; 0: not since power-on */
    uint16_t fw_build;         /* outdoor unit's firmware build number */
    uint8_t  reset_reason;     /* esp_reset_reason() of the current boot */
    uint8_t  flags;            /* WS_OUTDOOR_* */
    /* counters since the display last acknowledged a frame (MAC-level ack,
     * not the reply: a lost reply mustn't report the same wakes twice); a
     * later frame in the same wake carries zeros */
    uint32_t awake_total_ms;   /* summed wake lengths; the current wake isn't finished, so not included */
    uint8_t  brownouts;        /* brownout resets, saturating */
    uint8_t  resets;           /* other resets but power-on and deep-sleep wakes (panic, watchdog, ...), saturating */
    uint8_t  failed_searches;  /* channel searches that found nothing, saturating */
    uint8_t  count;            /* records that follow, 1..WS_LINK_MAX_RECORDS */
    ws_link_record_t rec[];    /* oldest first */
} ws_link_readings_t;

#define WS_LINK_READINGS_SIZE(n) (sizeof(ws_link_readings_t) + (n) * sizeof(ws_link_record_t))

/* display -> outdoor unit */
typedef struct __attribute__((packed)) {
    ws_link_hdr_t hdr;         /* type = WS_LINK_REPLY, seq echoed */
    uint32_t unix_now;         /* display clock (NTP), UTC seconds */
    uint16_t interval_s;       /* measurement interval from now on */
    uint8_t  cmd;              /* WS_CMD_* */
    /* sent only when cmd == WS_CMD_OTA */
    uint8_t  ota_ip[4];        /* display IPv4 address */
    uint32_t ota_size;         /* image size in bytes */
    uint8_t  ota_sha256[32];   /* image hash */
} ws_link_reply_t;

#define WS_LINK_REPLY_SIZE         offsetof(ws_link_reply_t, ota_ip)
#define WS_LINK_REPLY_OTA_SIZE     sizeof(ws_link_reply_t)

/* A reading's time on the right time base. The outdoor unit's clock runs at
 * a slightly wrong but steady rate (0.76 % fast on the bench), so its error
 * grows from 0 when a reply set it (set_at, on its own clock) to offset
 * (right time minus its clock) at now. Readings from before set_at need
 * nothing; with set_at 0 (not set since power-on) every reading moves by the
 * whole offset. Both sides use this: the display for the frame it gets
 * before its reply, the outdoor unit for its backlog when a reply sets its
 * clock. */
static inline uint32_t ws_link_correct_ts(uint32_t ts, int64_t offset, uint32_t set_at, uint32_t now)
{
    int64_t shift = offset;
    if (set_at != 0 && now > set_at) {
        shift = ts <= set_at ? 0 : offset * (int64_t)(ts - set_at) / (int64_t)(now - set_at);
    }
    const int64_t t = (int64_t)ts + shift;
    return t < 0 ? 0 : t > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)t;
}

/* the wire format: any change here is a protocol change (bump the version) */
_Static_assert(sizeof(ws_link_hdr_t) == 6, "ws_link_hdr_t must be 6 bytes");
_Static_assert(sizeof(ws_link_record_t) == 17, "ws_link_record_t must be 17 bytes");
_Static_assert(sizeof(ws_link_readings_t) == 26, "ws_link_readings_t header must be 26 bytes");
_Static_assert(WS_LINK_READINGS_SIZE(WS_LINK_MAX_RECORDS) <= WS_LINK_MAX_PAYLOAD,
               "a full READINGS frame must fit one ESP-NOW payload");
_Static_assert(WS_LINK_REPLY_SIZE == 13, "REPLY without OTA must be 13 bytes");
_Static_assert(WS_LINK_REPLY_OTA_SIZE == 53, "REPLY with OTA must be 53 bytes");

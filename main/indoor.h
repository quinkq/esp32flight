#pragma once

/* The indoor SHT40 on the display's I2C header (weather-station fork). */

#include <stdbool.h>
#include <stdint.h>

/* Once, after the board's I2C bus and the readings store are up. */
void indoor_start(void);

typedef struct {
    bool     answering;        /* false after 3 failed samples in a row */
    bool     have;             /* a sample since the display started */
    uint32_t age_s;            /* since that sample */
    int16_t  t;                /* the latest sample, raw: 0.01 °C */
    uint16_t rh;               /* 0.01 %RH */
    uint32_t serial;           /* the sensor's serial number; 0: not read */
} indoor_t;

void indoor_latest(indoor_t *out);

#pragma once

/* The weather station's calibration, applied when values are read: the store
 * keeps them raw, so a changed offset corrects the whole history
 * (weather-station fork). */

#include <stdbool.h>
#include <stdint.h>
#include "readings_store.h"

typedef struct {
    int16_t out_t, out_rh, in_t, in_rh;  /* hundredths */
    int16_t bat_trim_pm;
    bool    have_alt;
    int     alt_m;
} calibration_t;

/* The current settings, taken once for a batch of values. */
void calibration_get(calibration_t *c);

/* A stored slot as shown: offsets added (humidity kept within 0-100 %), the
 * battery voltage scaled, pressure reduced to sea level when the altitude is
 * known. */
void calibration_apply(const calibration_t *c, readings_slot_t *s);

/* The indoor sensor's latest sample as shown. */
void calibration_indoor(const calibration_t *c, int16_t *t, uint16_t *rh);

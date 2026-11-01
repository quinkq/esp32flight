#pragma once

#include <stdint.h>

typedef enum {
    BATTERY_OK,                /* radio on every wake */
    BATTERY_LOW,               /* below 3.4 V: radio every 4th wake */
    BATTERY_EMPTY,             /* below 3.1 V: radio off until back above 3.3 V */
} battery_mode_t;

/* Cell voltage in mV, or WS_LINK_NO_U16 if the reading failed or makes no
 * sense (a divider fault). Call before the radio starts. */
uint16_t battery_read_mv(void);

/* The mode for this wake. An unknown voltage counts as OK, so a broken
 * divider shows up on the display instead of silencing the outdoor unit. */
battery_mode_t battery_mode(uint16_t mv);

#pragma once

#include <stdint.h>

/* First thing in app_main: LED pins off, brownout guard (after a brownout
 * reset it deep-sleeps and never returns), hard-cap timer. */
void power_begin(void);

/* Brownout resets since the last successful exchange, saturating at 255. */
uint8_t power_brownouts(void);

/* End of a normal wake: deep sleep until the next aligned slot. */
void power_sleep(void) __attribute__((noreturn));

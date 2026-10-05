#pragma once

#include <stdbool.h>
#include <stdint.h>

/* First thing in app_main: LED pins off, brownout guard (after a brownout
 * reset it deep-sleeps and never returns), hard-cap timer. */
void power_begin(void);

/* Brownout resets since the last successful exchange, saturating at 255. */
uint8_t power_brownouts(void);

/* A successful exchange: the brownout count has been reported. */
void power_exchange_done(void);

/* Sets the clock from the display; returns new minus old, in seconds. From
 * then on the clock counts as synced until the next power-on. */
int64_t power_set_clock(uint32_t unix_now);
bool power_clock_synced(void);

/* The measurement interval from the display. Ignored unless it's 60-900 s
 * and divides 900 (the display's 15-minute grid). */
void power_set_interval(uint16_t interval_s);
uint16_t power_interval(void);

/* Length of the previous wake, ms (app start to sleep). */
uint16_t power_last_awake_ms(void);

/* End of a normal wake: deep sleep until the next aligned slot. */
void power_sleep(void) __attribute__((noreturn));

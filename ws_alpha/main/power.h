#pragma once

#include <stdbool.h>
#include <stdint.h>

/* First thing in app_main: LED pins off, brownout guard (after a brownout
 * reset it deep-sleeps and never returns), hard-cap timer. */
void power_begin(void);

/* Reported to the display, each since it last acknowledged a frame: brownout
 * resets and other resets (not power-on or deep-sleep wakes), both saturating
 * at 255, and the summed length of the wakes (app start to sleep), the
 * current one not included. */
uint8_t power_brownouts(void);
uint8_t power_resets(void);
uint32_t power_awake_total_ms(void);

/* The display acknowledged a frame carrying the counts above: they restart. */
void power_frame_acked(void);

/* Sets the clock from the display; returns new minus old, in seconds. From
 * then on the clock counts as synced until the next power-on. */
int64_t power_set_clock(uint32_t unix_now);
bool power_clock_synced(void);

/* The clock's value when the display last set it; 0 if not since power-on. */
uint32_t power_clock_set_at(void);

/* The measurement interval from the display. Ignored unless it's 60 s to
 * 6 h. */
void power_set_interval(uint16_t interval_s);
uint16_t power_interval(void);

/* End of a normal wake: deep sleep until the next aligned slot. */
void power_sleep(void) __attribute__((noreturn));

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ws_link.h"

#define BACKLOG_CAPACITY 192       /* 2 days at 15 min */

/* Once per wake, before anything else touches the backlog: validates it
 * after a reset and drops it if the clock went backwards (the old records
 * can no longer be dated). */
void backlog_begin(uint32_t now);

/* Appends a record; when full, the oldest one is dropped first. */
void backlog_push(const ws_link_record_t *rec);

uint16_t backlog_count(void);

/* Copies up to max of the oldest records into out without removing them;
 * returns how many. out may be unaligned (a packed frame). */
uint16_t backlog_peek(ws_link_record_t *out, uint16_t max);

/* Removes the n oldest records. */
void backlog_drop(uint16_t n);

/* Called when the display sets the clock, so the records still waiting stay
 * on the same time base as the new clock: delta is new minus old, set_at the
 * clock's value when it was last set (0: not since power-on), now its old
 * value. Each record moves by its share of delta (ws_link_correct_ts()). */
void backlog_shift(int64_t delta, uint32_t set_at, uint32_t now);

/* Records were lost (full backlog or a lost time base) since the flag was
 * last cleared, i.e. since the last successful exchange. */
bool backlog_dropped(void);
void backlog_clear_dropped(void);

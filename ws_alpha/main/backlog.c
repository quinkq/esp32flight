/*
 * Readings not yet confirmed by the display, oldest first, in a ring in RTC
 * memory (192 x 17 bytes of the C3's 8 KB). RTC_NOINIT survives deep sleep
 * and resets other than a power-on, so a brownout at the radio's first burst
 * doesn't lose the wake's reading. After a power loss the ring is garbage;
 * the magic (which includes the layout's size, so a firmware with a changed
 * layout starts afresh) and the range checks catch that.
 */
#include "backlog.h"

#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "backlog";

#define MAGIC_BASE 0x57534200u

typedef struct {
    uint32_t magic;
    uint16_t head;             /* index of the oldest record */
    uint16_t count;
    bool     dropped;
    ws_link_record_t rec[BACKLOG_CAPACITY];
} backlog_t;

#define MAGIC (MAGIC_BASE ^ (uint32_t)sizeof(backlog_t))

static RTC_NOINIT_ATTR backlog_t s_log;

static ws_link_record_t *at(uint16_t i)   /* i-th oldest */
{
    return &s_log.rec[(s_log.head + i) % BACKLOG_CAPACITY];
}

void backlog_begin(uint32_t now)
{
    if (esp_reset_reason() == ESP_RST_POWERON || s_log.magic != MAGIC ||
        s_log.head >= BACKLOG_CAPACITY || s_log.count > BACKLOG_CAPACITY) {
        memset(&s_log, 0, sizeof(s_log));
        s_log.magic = MAGIC;
        return;
    }
    if (s_log.count > 0 && at(s_log.count - 1)->ts > now + 60) {
        ESP_LOGW(TAG, "clock went backwards (newest record %lu, now %lu): dropping %u records",
                 (unsigned long)at(s_log.count - 1)->ts, (unsigned long)now, s_log.count);
        s_log.count = 0;
        s_log.dropped = true;
    }
}

void backlog_push(const ws_link_record_t *rec)
{
    if (s_log.count == BACKLOG_CAPACITY) {
        backlog_drop(1);
        s_log.dropped = true;
    }
    memcpy(at(s_log.count), rec, sizeof(*rec));
    s_log.count++;
}

uint16_t backlog_count(void)
{
    return s_log.count;
}

uint16_t backlog_peek(ws_link_record_t *out, uint16_t max)
{
    const uint16_t n = s_log.count < max ? s_log.count : max;
    for (uint16_t i = 0; i < n; i++) {
        memcpy(&out[i], at(i), sizeof(ws_link_record_t));
    }
    return n;
}

void backlog_drop(uint16_t n)
{
    if (n > s_log.count) {
        n = s_log.count;
    }
    s_log.head = (s_log.head + n) % BACKLOG_CAPACITY;
    s_log.count -= n;
}

void backlog_shift(int64_t delta)
{
    for (uint16_t i = 0; i < s_log.count; i++) {
        ws_link_record_t *r = at(i);
        const int64_t ts = (int64_t)r->ts + delta;
        r->ts = ts < 0 ? 0 : ts > UINT32_MAX ? UINT32_MAX : (uint32_t)ts;
    }
}

bool backlog_dropped(void)
{
    return s_log.dropped;
}

void backlog_clear_dropped(void)
{
    s_log.dropped = false;
}

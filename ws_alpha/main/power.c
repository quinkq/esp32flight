/*
 * The frame around each wake.
 *
 *  - The kit's white LEDs sit on GPIO18/19, which are also the C3's USB
 *    pins. Both are driven low; on GPIO19 that also drops the USB D+
 *    pull-up, which otherwise lights the LED while awake.
 *  - Brownout guard: a brownout reset starts nothing that draws current.
 *    It deep-sleeps instead, longer after each brownout in a row, so a weak
 *    supply can't keep the outdoor unit looping at ~70 mA until the cell is
 *    empty (that's how the previous cell died).
 *  - Hard cap: whatever hangs, the outdoor unit is asleep 3 s after boot.
 *  - Wakes are aligned to multiples of the interval (:00, :15, ...); the
 *    display can change the interval and sets the clock.
 *  - Counts for the display, since it last acknowledged a frame: brownouts,
 *    other resets, time awake. Failed wakes are reported this way too.
 */
#include "power.h"

#include <inttypes.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "power";

#define HARD_CAP_MS  3000
#define MIN_SLEEP_S  30            /* closer to a slot than this: take the next one */
#define MAGIC_BASE   0x57535057u

/* sleep after brownouts in a row, seconds; the last value repeats */
static const uint32_t k_backoff_s[] = { 10, 60, 600, 3600 };
#define BACKOFF_STEPS (sizeof(k_backoff_s) / sizeof(k_backoff_s[0]))

/* Survives deep sleep and brownout or panic resets (unlike RTC_DATA_ATTR,
 * which the bootloader reloads after any reset but a deep-sleep wake).
 * Garbage after power-on, hence the magic; it includes the layout's size,
 * so a firmware with a changed layout starts afresh. The clock itself lives
 * in the RTC and survives the same resets. */
typedef struct {
    uint32_t magic;
    uint8_t  in_a_row;         /* brownouts without a clean wake in between */
    bool     clock_synced;
    uint32_t clock_set_at;     /* the clock's value when the display last set it */
    uint16_t interval_s;
    /* reported to the display; restart when it acknowledges a frame */
    uint8_t  brownouts;
    uint8_t  resets;
    uint32_t awake_total_ms;
} power_state_t;

#define STATE_MAGIC (MAGIC_BASE ^ (uint32_t)sizeof(power_state_t))

static RTC_NOINIT_ATTR power_state_t s_state;

static void count_up(uint8_t *n)
{
    if (*n < UINT8_MAX) {
        (*n)++;
    }
}

static void add_awake(uint32_t ms)
{
    s_state.awake_total_ms = ms > UINT32_MAX - s_state.awake_total_ms ? UINT32_MAX : s_state.awake_total_ms + ms;
}

static void leds_off(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << GPIO_NUM_18) | (1ULL << GPIO_NUM_19),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_set_level(GPIO_NUM_18, 0);
    gpio_set_level(GPIO_NUM_19, 0);
    gpio_config(&cfg);
}

static uint64_t us_to_next_slot(void)
{
    const int64_t interval_us = (int64_t)s_state.interval_s * 1000000;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    const int64_t now_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;

    int64_t sleep_us = interval_us - now_us % interval_us;
    if (sleep_us < (int64_t)MIN_SLEEP_S * 1000000) {
        sleep_us += interval_us;   /* woke a little early: this slot is done */
    }
    return (uint64_t)sleep_us;
}

static void hard_cap_cb(void *arg)
{
    ESP_LOGW(TAG, "hard cap: still awake after %d ms, forcing sleep", HARD_CAP_MS);
    add_awake(HARD_CAP_MS);
    esp_deep_sleep(us_to_next_slot());
}

void power_begin(void)
{
    leds_off();

    const esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON || s_state.magic != STATE_MAGIC) {
        s_state = (power_state_t){
            .magic = STATE_MAGIC,
            .interval_s = CONFIG_WS_ALPHA_INTERVAL_S,
        };
    }

    if (reason == ESP_RST_BROWNOUT) {
        count_up(&s_state.in_a_row);
        count_up(&s_state.brownouts);
        const size_t step = s_state.in_a_row <= BACKOFF_STEPS ? s_state.in_a_row - 1 : BACKOFF_STEPS - 1;
        ESP_LOGW(TAG, "brownout reset, %u in a row: sleeping %" PRIu32 " s",
                 s_state.in_a_row, k_backoff_s[step]);
        esp_deep_sleep((uint64_t)k_backoff_s[step] * 1000000);
    }
    if (reason != ESP_RST_POWERON && reason != ESP_RST_DEEPSLEEP) {
        count_up(&s_state.resets);
        ESP_LOGW(TAG, "reset (reason %d), %u since the display last heard us", reason, s_state.resets);
    }

    const esp_timer_create_args_t args = { .callback = hard_cap_cb, .name = "hard_cap" };
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) != ESP_OK ||
        esp_timer_start_once(timer, (uint64_t)HARD_CAP_MS * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "no hard-cap timer");
    }
}

uint8_t power_brownouts(void)
{
    return s_state.brownouts;
}

uint8_t power_resets(void)
{
    return s_state.resets;
}

uint32_t power_awake_total_ms(void)
{
    return s_state.awake_total_ms;
}

void power_frame_acked(void)
{
    s_state.brownouts = 0;
    s_state.resets = 0;
    s_state.awake_total_ms = 0;
}

int64_t power_set_clock(uint32_t unix_now)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    const int64_t delta = (int64_t)unix_now - tv.tv_sec;
    tv = (struct timeval){ .tv_sec = unix_now };
    settimeofday(&tv, NULL);
    s_state.clock_synced = true;
    s_state.clock_set_at = unix_now;
    return delta;
}

bool power_clock_synced(void)
{
    return s_state.clock_synced;
}

uint32_t power_clock_set_at(void)
{
    return s_state.clock_synced ? s_state.clock_set_at : 0;
}

void power_set_interval(uint16_t interval_s)
{
    if (interval_s == s_state.interval_s) {
        return;
    }
    /* any value: the display's store takes readings to the nearest 15-minute
     * slot (averaged below 15 min, gaps between slots above) */
    if (interval_s < 60 || interval_s > 6 * 3600) {
        ESP_LOGW(TAG, "ignoring interval %u s from the display", interval_s);
        return;
    }
    ESP_LOGI(TAG, "interval %u s -> %u s", s_state.interval_s, interval_s);
    s_state.interval_s = interval_s;
}

uint16_t power_interval(void)
{
    return s_state.interval_s;
}

void power_sleep(void)
{
    s_state.in_a_row = 0;
    const int64_t awake_ms = esp_timer_get_time() / 1000;
    add_awake((uint32_t)awake_ms);
    const uint64_t sleep_us = us_to_next_slot();
    /* esp_timer starts after the ROM and the bootloader (~0.1-0.3 s), so this
     * undercounts the whole wake by about that much */
    ESP_LOGI(TAG, "awake %lld ms, next wake in %llu s", awake_ms, sleep_us / 1000000);
    esp_deep_sleep(sleep_us);
}

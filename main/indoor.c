/*
 * The indoor SHT40 on the display's I2C header (weather-station fork;
 * "Indoor SHT40" in DISPLAY.md, in the planning notes).
 *
 *  - Upstream's legacy I2C driver on the board's bus, which the touch
 *    controller and the IO helper share; the driver locks each transaction.
 *    About 1 ms of bus time per sample.
 *  - A sample every 60 s: high repeatability, heater never used, CRC checked.
 *  - The samples of a slot (the store's rule: the nearest slot start) are
 *    averaged here; the mean goes to the store once a sample belongs to the
 *    next slot, so flash sees one write per slot rather than one per minute.
 *    A display restart loses the half-filled slot. Until the display has
 *    time, samples only update the latest value.
 *  - Lost after 3 failed samples in a row, back after 3 good ones: an
 *    "indoor" event in the weather station's event log.
 */
#include "indoor.h"

#include <string.h>
#include "sdkconfig.h"

#if !defined(APKFLIGHT) && !CONFIG_IDF_TARGET_ESP32P4

#include <time.h>
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "outdoor_link.h"
#include "readings_store.h"
#include "waveshare_rgb_lcd_port.h"

static const char *TAG = "indoor";

#define SHT40_ADDR       0x44
#define CMD_MEASURE      0xFD           /* T and RH, high repeatability, no heater */
#define CMD_SERIAL       0x89
#define MEASURE_MS       10             /* the datasheet's maximum: 8.3 ms */
#define SERIAL_MS        2
#define I2C_WAIT_MS      50             /* for the bus, which others share */
#define SAMPLE_MS        60000
#define RUN              3              /* samples in a row before it counts as lost or back */
#define CLOCK_VALID_FROM 1767225600     /* 2026-01-01, as in outdoor-link: earlier means no time yet */
#define TASK_STACK       4096
#define TASK_PRIORITY    2

static SemaphoreHandle_t s_mux;         /* guards s_latest and s_latest_us */
static indoor_t s_latest = { .answering = true };
static int64_t s_latest_us;

/* the task's own */
static uint8_t s_run;                   /* results in a row that disagree with s_latest.answering */
static uint32_t s_slot;                 /* the slot being averaged; 0: none */
static int32_t s_sum_t, s_sum_rh, s_n;

/* the SHT4x's CRC: polynomial 0x31, start 0xFF, over one 2-byte word */
static uint8_t crc8(const uint8_t *d)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < 2; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = crc & 0x80 ? (uint8_t)(crc << 1 ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* A command, then its answer: two words, each followed by its CRC. */
static bool command(uint8_t cmd, int wait_ms, uint8_t out[6])
{
    if (i2c_master_write_to_device(I2C_MASTER_NUM, SHT40_ADDR, &cmd, 1, pdMS_TO_TICKS(I2C_WAIT_MS)) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
    return i2c_master_read_from_device(I2C_MASTER_NUM, SHT40_ADDR, out, 6, pdMS_TO_TICKS(I2C_WAIT_MS)) == ESP_OK &&
           crc8(out) == out[2] && crc8(out + 3) == out[5];
}

/* rounded to the nearest */
static int32_t mean(int32_t sum, int32_t n)
{
    return sum >= 0 ? (sum + n / 2) / n : -((-sum + n / 2) / n);
}

/* A sample into its slot's mean; a slot's mean goes to the store (and
 * flash) once a sample belongs to the next one. */
static void average(uint32_t now, int16_t t, uint16_t rh)
{
    const uint32_t slot = (now + READINGS_SLOT_S / 2) / READINGS_SLOT_S;
    if (slot != s_slot) {
        if (s_n > 0) {
            readings_store_put_indoor(s_slot * READINGS_SLOT_S, (int16_t)mean(s_sum_t, s_n),
                                      (uint16_t)mean(s_sum_rh, s_n));
            readings_store_flush();
        }
        s_slot = slot;
        s_sum_t = s_sum_rh = s_n = 0;
    }
    s_sum_t += t;
    s_sum_rh += rh;
    s_n++;
}

/* answering or not, with an event once it has held for RUN samples */
static void track(bool ok)
{
    if (ok == s_latest.answering) {     /* only this task writes it */
        s_run = 0;
        return;
    }
    if (++s_run < RUN) {
        return;
    }
    s_run = 0;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_latest.answering = ok;
    xSemaphoreGive(s_mux);
    outdoor_link_event("indoor", ok ? "indoor SHT40 answers again"
                                    : "indoor SHT40 doesn't answer (3 samples in a row)");
}

static void sample(void)
{
    uint8_t d[6];
    const bool ok = command(CMD_MEASURE, MEASURE_MS, d);
    track(ok);
    if (!ok) {
        return;
    }
    /* the datasheet's formulas, in hundredths */
    const uint32_t raw_t = (uint32_t)d[0] << 8 | d[1], raw_rh = (uint32_t)d[3] << 8 | d[4];
    const int16_t t = (int16_t)(-4500 + (int32_t)((17500u * raw_t + 32767) / 65535));
    int32_t rh = -600 + (int32_t)((12500u * raw_rh + 32767) / 65535);
    rh = rh < 0 ? 0 : rh > 10000 ? 10000 : rh;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_latest.have = true;
    s_latest.t = t;
    s_latest.rh = (uint16_t)rh;
    s_latest_us = esp_timer_get_time();
    xSemaphoreGive(s_mux);

    const time_t now = time(NULL);
    if (now >= CLOCK_VALID_FROM) {
        average((uint32_t)now, t, (uint16_t)rh);
    }
}

static void task(void *arg)
{
    uint8_t d[6];
    if (command(CMD_SERIAL, SERIAL_MS, d)) {
        const uint32_t serial = (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[3] << 8 | d[4];
        xSemaphoreTake(s_mux, portMAX_DELAY);
        s_latest.serial = serial;
        xSemaphoreGive(s_mux);
        ESP_LOGI(TAG, "SHT40 at 0x%02x, serial %08lx", SHT40_ADDR, (unsigned long)serial);
    } else {
        ESP_LOGW(TAG, "no SHT40 at 0x%02x yet; trying every minute", SHT40_ADDR);
    }
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        sample();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_MS));
    }
}

void indoor_start(void)
{
    s_mux = xSemaphoreCreateMutex();
    if (s_mux == NULL || xTaskCreate(task, "indoor", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the indoor sensor's task");
        s_latest.answering = false;
    }
}

void indoor_latest(indoor_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_mux == NULL) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_latest;
    const int64_t at_us = s_latest_us;
    xSemaphoreGive(s_mux);
    if (out->have) {
        out->age_s = (uint32_t)((esp_timer_get_time() - at_us) / 1000000);
    }
}

#else

void indoor_start(void)
{
}

void indoor_latest(indoor_t *out)
{
    memset(out, 0, sizeof(*out));
}

#endif

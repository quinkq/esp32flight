/*
 * SHT40 (0x44) and BME280 (0x76) on one I2C bus: SDA GPIO6, SCL GPIO7,
 * 100 kHz, pull-ups on the breakout boards (10 kΩ on each).
 *
 * Both sensors measure in parallel (~10 ms). Errors get short, bounded
 * retries: 20 ms per transaction, at most 3 attempts per sensor, with a bus
 * reset and the sensor's soft reset in between. A missing sensor NACKs
 * within a millisecond; only a stuck bus runs into the timeouts (~0.5 s at
 * worst, well inside the hard cap).
 */
#include "sensors.h"

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sensors";

#define I2C_SDA          GPIO_NUM_6
#define I2C_SCL          GPIO_NUM_7
#define I2C_HZ           100000
#define XFER_TIMEOUT_MS  20
#define ATTEMPTS         3
#define MEAS_WAIT_MS     12        /* max: SHT40 8.3 ms, BME280 9.3 ms at x1/x1/x1 */

#define SHT40_ADDR       0x44
#define SHT40_MEASURE    0xFD      /* T and RH, high repeatability, heater off */
#define SHT40_SOFT_RESET 0x94

#define BME280_ADDR      0x76
#define BME280_CHIP_ID   0x60
#define BME280_REG_ID    0xD0
#define BME280_REG_RESET 0xE0
#define BME280_RESET_CMD 0xB6
#define BME280_REG_CAL1  0x88      /* 26 bytes: T1..P9, one unused, H1 */
#define BME280_REG_CAL2  0xE1      /* 7 bytes: H2..H6 */
#define BME280_REG_DATA  0xF7      /* 8 bytes: pressure, temperature, humidity */

typedef struct {
    uint16_t t1;
    int16_t  t2, t3;
    uint16_t p1;
    int16_t  p2, p3, p4, p5, p6, p7, p8, p9;
    uint8_t  h1, h3;
    int16_t  h2, h4, h5;
    int8_t   h6;
} bme280_calib_t;

/* The calibration is factory-fixed, so it's read once and kept across deep
 * sleep (reloaded after any other reset). */
static RTC_DATA_ATTR bme280_calib_t s_calib;
static RTC_DATA_ATTR bool s_calib_valid;

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_sht, s_bme;

static esp_err_t bus_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,   /* the boards carry their own */
    };
    /* the driver warns about the disabled internal pull-ups on every wake
     * (~15 ms of console output); transfer errors are logged as errors */
    esp_log_level_set("i2c.master", ESP_LOG_ERROR);
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        return err;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT40_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_sht);
    if (err != ESP_OK) {
        return err;
    }
    dev_cfg.device_address = BME280_ADDR;
    return i2c_master_bus_add_device(s_bus, &dev_cfg, &s_bme);
}

/* ---------- SHT40 ---------- */

static uint8_t sht40_crc(const uint8_t *data)   /* CRC-8, poly 0x31, init 0xFF, over 2 bytes */
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < 2; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t sht40_start(void)
{
    const uint8_t cmd = SHT40_MEASURE;
    return i2c_master_transmit(s_sht, &cmd, 1, XFER_TIMEOUT_MS);
}

static esp_err_t sht40_fetch(ws_link_record_t *rec)
{
    uint8_t d[6];   /* T msb, lsb, crc, RH msb, lsb, crc */
    const esp_err_t err = i2c_master_receive(s_sht, d, sizeof(d), XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    if (sht40_crc(d) != d[2] || sht40_crc(d + 3) != d[5]) {
        return ESP_ERR_INVALID_CRC;
    }
    const int32_t raw_t = (d[0] << 8) | d[1];
    const int32_t raw_rh = (d[3] << 8) | d[4];

    /* datasheet: T = -45 + 175 * raw / 65535 °C, RH = -6 + 125 * raw / 65535 %, here in 0.01 */
    int32_t rh = -600 + 12500 * raw_rh / 65535;
    rh = rh < 0 ? 0 : rh > 10000 ? 10000 : rh;
    rec->sht_t = (int16_t)(-4500 + 17500 * raw_t / 65535);
    rec->sht_rh = (uint16_t)rh;
    rec->flags |= WS_REC_SHT_OK;
    return ESP_OK;
}

static void sht40_soft_reset(void)
{
    const uint8_t cmd = SHT40_SOFT_RESET;
    i2c_master_transmit(s_sht, &cmd, 1, XFER_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(1));
}

/* ---------- BME280 ---------- */

static esp_err_t bme280_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_bme, &reg, 1, buf, len, XFER_TIMEOUT_MS);
}

static esp_err_t bme280_load_calib(void)
{
    uint8_t id, a[26], b[7];
    esp_err_t err = bme280_read(BME280_REG_ID, &id, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (id != BME280_CHIP_ID) {
        ESP_LOGW(TAG, "chip id 0x%02x at 0x76: not a BME280", id);
        return ESP_ERR_NOT_FOUND;
    }
    if ((err = bme280_read(BME280_REG_CAL1, a, sizeof(a))) != ESP_OK ||
        (err = bme280_read(BME280_REG_CAL2, b, sizeof(b))) != ESP_OK) {
        return err;
    }

    bme280_calib_t *c = &s_calib;
    c->t1 = (uint16_t)(a[1] << 8 | a[0]);
    c->t2 = (int16_t)(a[3] << 8 | a[2]);
    c->t3 = (int16_t)(a[5] << 8 | a[4]);
    c->p1 = (uint16_t)(a[7] << 8 | a[6]);
    c->p2 = (int16_t)(a[9] << 8 | a[8]);
    c->p3 = (int16_t)(a[11] << 8 | a[10]);
    c->p4 = (int16_t)(a[13] << 8 | a[12]);
    c->p5 = (int16_t)(a[15] << 8 | a[14]);
    c->p6 = (int16_t)(a[17] << 8 | a[16]);
    c->p7 = (int16_t)(a[19] << 8 | a[18]);
    c->p8 = (int16_t)(a[21] << 8 | a[20]);
    c->p9 = (int16_t)(a[23] << 8 | a[22]);
    c->h1 = a[25];                                           /* 0xA1 */
    c->h2 = (int16_t)(b[1] << 8 | b[0]);                     /* 0xE1/0xE2 */
    c->h3 = b[2];                                            /* 0xE3 */
    c->h4 = (int16_t)((int8_t)b[3] * 16 | (b[4] & 0x0F));    /* 0xE4 << 4 | 0xE5 low nibble */
    c->h5 = (int16_t)((int8_t)b[5] * 16 | (b[4] >> 4));      /* 0xE6 << 4 | 0xE5 high nibble */
    c->h6 = (int8_t)b[6];                                    /* 0xE7 */
    s_calib_valid = true;
    return ESP_OK;
}

static esp_err_t bme280_start(void)
{
    if (!s_calib_valid) {
        const esp_err_t err = bme280_load_calib();
        if (err != ESP_OK) {
            return err;
        }
    }
    /* register/value pairs: humidity x1 (takes effect with the ctrl_meas
     * write that follows), then temperature x1, pressure x1, forced mode */
    const uint8_t cmd[] = { 0xF2, 0x01, 0xF4, 0x25 };
    return i2c_master_transmit(s_bme, cmd, sizeof(cmd), XFER_TIMEOUT_MS);
}

/* Bosch's integer compensation (BME280 datasheet, section 4.2.3) */

static int32_t bme280_t_fine(int32_t adc_t)
{
    const bme280_calib_t *c = &s_calib;
    const int32_t var1 = ((((adc_t >> 3) - ((int32_t)c->t1 << 1))) * c->t2) >> 11;
    const int32_t var2 = (((((adc_t >> 4) - (int32_t)c->t1) * ((adc_t >> 4) - (int32_t)c->t1)) >> 12) * c->t3) >> 14;
    return var1 + var2;
}

static uint32_t bme280_pressure_q24_8(int32_t adc_p, int32_t t_fine)   /* Pa * 256 */
{
    const bme280_calib_t *c = &s_calib;
    int64_t var1 = (int64_t)t_fine - 128000;
    int64_t var2 = var1 * var1 * c->p6;
    var2 = var2 + ((var1 * c->p5) << 17);
    var2 = var2 + ((int64_t)c->p4 << 35);
    var1 = ((var1 * var1 * c->p3) >> 8) + ((var1 * c->p2) << 12);
    var1 = (((int64_t)1 << 47) + var1) * c->p1 >> 33;
    if (var1 == 0) {
        return 0;
    }
    int64_t p = 1048576 - adc_p;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = ((int64_t)c->p9 * (p >> 13) * (p >> 13)) >> 25;
    var2 = ((int64_t)c->p8 * p) >> 19;
    p = ((p + var1 + var2) >> 8) + ((int64_t)c->p7 << 4);
    return (uint32_t)p;
}

static uint32_t bme280_humidity_q22_10(int32_t adc_h, int32_t t_fine)   /* %RH * 1024 */
{
    const bme280_calib_t *c = &s_calib;
    int32_t v = t_fine - 76800;
    v = (((((adc_h << 14) - ((int32_t)c->h4 << 20) - ((int32_t)c->h5 * v)) + 16384) >> 15) *
         (((((((v * c->h6) >> 10) * (((v * (int32_t)c->h3) >> 11) + 32768)) >> 10) + 2097152) *
           c->h2 + 8192) >> 14));
    v = v - (((((v >> 15) * (v >> 15)) >> 7) * (int32_t)c->h1) >> 4);
    v = v < 0 ? 0 : v > 419430400 ? 419430400 : v;
    return (uint32_t)(v >> 12);
}

static esp_err_t bme280_fetch(ws_link_record_t *rec)
{
    uint8_t d[8];
    const esp_err_t err = bme280_read(BME280_REG_DATA, d, sizeof(d));
    if (err != ESP_OK) {
        return err;
    }
    const int32_t adc_p = (int32_t)d[0] << 12 | d[1] << 4 | d[2] >> 4;
    const int32_t adc_t = (int32_t)d[3] << 12 | d[4] << 4 | d[5] >> 4;
    const int32_t adc_h = (int32_t)d[6] << 8 | d[7];
    if (adc_t == 0x80000 || adc_p == 0x80000 || adc_h == 0x8000) {
        return ESP_ERR_INVALID_STATE;   /* reset values: no measurement happened */
    }

    const int32_t t_fine = bme280_t_fine(adc_t);
    rec->bme_t = (int16_t)((t_fine * 5 + 128) >> 8);                                  /* 0.01 °C */
    rec->bme_p = (uint16_t)((bme280_pressure_q24_8(adc_p, t_fine) + 1280) / 2560);    /* 0.1 hPa */
    rec->bme_rh = (uint16_t)((bme280_humidity_q22_10(adc_h, t_fine) * 100 + 512) / 1024); /* 0.01 % */
    rec->flags |= WS_REC_BME_OK;
    return ESP_OK;
}

static void bme280_soft_reset(void)
{
    const uint8_t cmd[] = { BME280_REG_RESET, BME280_RESET_CMD };
    i2c_master_transmit(s_bme, cmd, sizeof(cmd), XFER_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(2));   /* start-up time */
}

/* ---------- both ---------- */

void sensors_read(ws_link_record_t *rec)
{
    rec->sht_t = WS_LINK_NO_I16;
    rec->sht_rh = WS_LINK_NO_U16;
    rec->bme_t = WS_LINK_NO_I16;
    rec->bme_rh = WS_LINK_NO_U16;
    rec->bme_p = WS_LINK_NO_U16;
    rec->flags &= ~(WS_REC_SHT_OK | WS_REC_BME_OK);

    if (bus_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus setup failed");
        return;
    }

    bool need_sht = true, need_bme = true;
    for (int attempt = 1; attempt <= ATTEMPTS && (need_sht || need_bme); attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "attempt %d:%s%s", attempt, need_sht ? " SHT40" : "", need_bme ? " BME280" : "");
            i2c_master_bus_reset(s_bus);
            if (need_sht) {
                sht40_soft_reset();
            }
            if (need_bme) {
                bme280_soft_reset();
            }
        }
        const bool sht_started = need_sht && sht40_start() == ESP_OK;
        const bool bme_started = need_bme && bme280_start() == ESP_OK;
        if (sht_started || bme_started) {
            vTaskDelay(pdMS_TO_TICKS(MEAS_WAIT_MS));
        }
        if (sht_started && sht40_fetch(rec) == ESP_OK) {
            need_sht = false;
        }
        if (bme_started && bme280_fetch(rec) == ESP_OK) {
            need_bme = false;
        }
    }
    if (need_sht) {
        ESP_LOGW(TAG, "SHT40 not answering");
    }
    if (need_bme) {
        ESP_LOGW(TAG, "BME280 not answering");
    }
}

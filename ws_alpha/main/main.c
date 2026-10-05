/*
 * ws_alpha: the solar outdoor station. One linear wake cycle, no tasks:
 *
 *   guard (brownout reset? sleep) -> battery -> sensors -> radio -> deep sleep
 *
 * The battery comes first: it's read before anything draws current, and it
 * decides whether the radio may start at all. The radio (backlog, ESP-NOW,
 * clock from the display) is the next stage; for now the record only goes
 * to the log.
 */
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "battery.h"
#include "power.h"
#include "sensors.h"
#include "ws_link.h"

static const char *TAG = "ws_alpha";

static const char *const k_mode_name[] = {
    [BATTERY_OK] = "ok",
    [BATTERY_LOW] = "low",
    [BATTERY_EMPTY] = "empty, radio off",
};

static void log_record(const ws_link_record_t *r, battery_mode_t mode)
{
    char sht[40] = "missing", bme[56] = "missing", bat[24] = "unknown";
    if (r->flags & WS_REC_SHT_OK) {
        snprintf(sht, sizeof(sht), "%.2f C %.2f %%RH", r->sht_t / 100.0, r->sht_rh / 100.0);
    }
    if (r->flags & WS_REC_BME_OK) {
        snprintf(bme, sizeof(bme), "%.2f C %.2f %%RH %.1f hPa",
                 r->bme_t / 100.0, r->bme_rh / 100.0, r->bme_p / 10.0);
    }
    if (r->bat_mv != WS_LINK_NO_U16) {
        snprintf(bat, sizeof(bat), "%u mV", r->bat_mv);
    }
    ESP_LOGI(TAG, "SHT40 %s | BME280 %s | battery %s (%s) | brownouts %u",
             sht, bme, bat, k_mode_name[mode], power_brownouts());
}

void app_main(void)
{
    power_begin();

    ws_link_record_t rec = { .ts = (uint32_t)time(NULL) };

    rec.bat_mv = battery_read_mv();
    const battery_mode_t mode = battery_mode(rec.bat_mv);

    sensors_read(&rec);
    log_record(&rec, mode);

    power_sleep();
}

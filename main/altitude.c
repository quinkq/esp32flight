/*
 * The altitude of the display's home location, for pressure at sea level
 * (weather-station fork; "Settings" in DISPLAY.md, in the planning notes).
 *
 *  - From Open-Meteo's elevation lookup (a 90 m terrain model; the service
 *    upstream already asks for the header weather), one request when the
 *    fixed home location is new, cached in the weather station settings.
 *  - Only for a fixed home location, as set on the web panel (exact) or by
 *    the touchscreen's city search (the city centre): an IP-based location
 *    can be kilometres off, and in hilly places that's a big error.
 *  - Upstream restarts the display when its settings change, so a check at
 *    boot catches every new home location.
 */
#include "altitude.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_util.h"
#include "settings.h"
#include "wifi_mgr.h"

static const char *TAG = "altitude";

#define SAME_E5       20                /* within this (×1e-5 degrees, ~20 m): the same place */
#define RETRY_MS      (10 * 60 * 1000)  /* after a failed lookup */
#define LOOKUP_MS     8000
#define TASK_STACK    8192              /* TLS */
#define TASK_PRIORITY 2

static int32_t e5(double deg)
{
    return (int32_t)lround(deg * 100000.0);
}

static bool same_place(int32_t lat_a, int32_t lon_a, int32_t lat_b, int32_t lon_b)
{
    return labs((long)(lat_a - lat_b)) <= SAME_E5 && labs((long)(lon_a - lon_b)) <= SAME_E5;
}

bool altitude_m(const ws_settings_t *cfg, int *m)
{
    if (cfg->alt_override_m != WS_ALT_AUTO) {
        *m = cfg->alt_override_m;
        return true;
    }
    const settings_t *home = settings_get();
    if (!home->use_fixed_loc || !cfg->alt_known ||
        !same_place(cfg->alt_lat_e5, cfg->alt_lon_e5, e5(home->lat), e5(home->lon))) {
        return false;
    }
    *m = cfg->alt_m;
    return true;
}

/* {"elevation":[12.0]} */
static bool lookup(double lat, double lon, int16_t *m)
{
    char url[128];
    snprintf(url, sizeof(url), "https://api.open-meteo.com/v1/elevation?latitude=%.5f&longitude=%.5f", lat, lon);
    char *buf = malloc(512);
    if (buf == NULL) {
        return false;
    }
    const esp_err_t err = http_get_to_buffer_t(url, buf, 512, NULL, LOOKUP_MS);
    cJSON *root = err == ESP_OK ? cJSON_Parse(buf) : NULL;
    free(buf);
    const cJSON *v = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "elevation"), 0);
    const bool ok = cJSON_IsNumber(v) && v->valuedouble > -500 && v->valuedouble < 9000;
    if (ok) {
        *m = (int16_t)lround(v->valuedouble);
    } else {
        ESP_LOGW(TAG, "lookup failed: %s", err != ESP_OK ? esp_err_to_name(err) : "no elevation in the answer");
    }
    cJSON_Delete(root);
    return ok;
}

static void task(void *arg)
{
    for (;;) {
        const settings_t *home = settings_get();
        if (!home->use_fixed_loc) {
            ESP_LOGI(TAG, "home location by IP address: pressure stays as measured");
            break;
        }
        const int32_t lat = e5(home->lat), lon = e5(home->lon);
        ws_settings_t cfg;
        ws_settings_get(&cfg);
        if (cfg.alt_known && same_place(cfg.alt_lat_e5, cfg.alt_lon_e5, lat, lon)) {
            ESP_LOGI(TAG, "%d m at %.5f, %.5f (looked up before)", cfg.alt_m, home->lat, home->lon);
            break;
        }
        int16_t m;
        if (wifi_mgr_wait_connected(60000) && lookup(home->lat, home->lon, &m)) {
            ws_settings_set_altitude(m, lat, lon);
            ESP_LOGI(TAG, "%d m at %.5f, %.5f (Open-Meteo)", m, home->lat, home->lon);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
    }
    vTaskDelete(NULL);
}

void altitude_start(void)
{
    if (xTaskCreate(task, "altitude", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the lookup; pressure stays as measured");
    }
}

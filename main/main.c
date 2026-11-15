#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "waveshare_rgb_lcd_port.h"
#include "lvgl.h"

#include "airports.h"
#include "flight_task.h"
#include "logos.h"
#include "settings.h"
#include "input_ctl.h"
#include "tilemap.h"
#include "ui.h"
#include "ui_settings.h"
#include "ui_map.h"
#include "web_server.h"
#include "wifi_mgr.h"
#include "outdoor_link.h"   /* weather-station fork: link to the outdoor unit */
#include "readings_store.h" /* weather-station fork: its readings, in RAM and their own partition */
#include "indoor.h"         /* weather-station fork: the indoor SHT40 */
#include "ws_settings.h"    /* weather-station fork: its own settings */
#include "altitude.h"       /* weather-station fork: for pressure at sea level */
#include "ota_guard.h"      /* weather-station fork: signed updates, rollback */

static const char *TAG = "canflight";

static void *psram_prefer_malloc(size_t n)
{
    return heap_caps_malloc_prefer(n, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_8BIT);
}

static void backlight_after_first_frame(lv_timer_t *t)
{
    (void)t;
    waveshare_rgb_lcd_bl_on();
    if (settings_get()->brightness_ctl && settings_get()->brightness < 100 &&
        waveshare_rgb_lcd_bl_dimmable()) {
        waveshare_rgb_lcd_bl_pct(settings_get()->brightness);
    }
}

void app_main(void)
{
    /* first thing in the log after any spontaneous restart: why it happened */
    ESP_LOGW("boot", "reset reason: %d (4=panic 5=int_wdt 6=task_wdt 7=other_wdt 9=brownout)",
             (int)esp_reset_reason());
    /* Local time for the clock and ETAs (Europe/Warsaw with DST) */
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    /* cJSON trees (state JSON, API parses) are large and transient:
     * keep them out of the scarce internal heap. */
    cJSON_Hooks jh = { .malloc_fn = psram_prefer_malloc, .free_fn = free };
    cJSON_InitHooks(&jh);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    settings_load();
    ws_settings_init();     /* weather-station fork */
    ota_guard_start();      /* weather-station fork: after settings_load() */

    ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init());

    logos_init();
    airports_init();
    tilemap_init();
    /* pre-decode both world maps so map opens never decode PNGs at draw time */
    ui_map_get_image();
    ui_map_get_image_small();

    if (lvgl_port_lock(-1)) {
        ui_init();
        if (settings_get()->wifi_ssid[0] == '\0') {
            ui_settings_open();   /* first boot: land straight in setup */
        }
        /* Light the panel only once it shows the first real frame: an
         * early bl_on lit an empty framebuffer, the white flash at every
         * boot. A one-shot LVGL timer runs in the LVGL task after its
         * first refresh. (Never lv_refr_now() from here: the RGB flush
         * waits for a vsync notification addressed to the LVGL task and
         * would block app_main forever.) */
        lv_timer_t *blt = lv_timer_create(backlight_after_first_frame, 250, NULL);
        lv_timer_set_repeat_count(blt, 1);
        lvgl_port_unlock();
    }

    readings_store_init();  /* weather-station fork: before outdoor_link_start() feeds it */
    indoor_start();         /* weather-station fork */
    err = wifi_mgr_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi init failed: %s", esp_err_to_name(err));
    } else {
        outdoor_link_start();
        altitude_start();   /* weather-station fork */
    }

    input_ctl_init();
    web_server_start();
    flight_task_start();
}

/*
 * Safe firmware updates for the display (weather-station fork).
 *
 * Signed builds (sdkconfig.weather-station): ESP-IDF accepts an OTA image
 * only with a valid signature from our key. It checks in esp_ota_end(), so
 * upstream's /ota handler answers "validate failed" to anything else. That
 * check, not upstream's arming switch ("Allow OTA updates (until restart)"),
 * now keeps foreign firmware out, so the switch is turned on at every start:
 * an update can come through the web panel at any time, also from afar
 * through the desktop. Switched off on the touchscreen, it still locks
 * updates until the next restart. A build without the signature check leaves
 * it off after a restart, as upstream has it.
 *
 * Rollback: after an update the new firmware starts on probation (ESP-IDF's
 * PENDING_VERIFY state). It confirms itself once it has run for 2 minutes
 * with Wi-Fi connected and the web panel up, so an update can't cut off the
 * way the next one comes in. Any restart before that (a crash, a watchdog
 * reset, power, but also a planned one such as saving settings) makes the
 * bootloader start the previous firmware. Not confirmed within 5 minutes:
 * back to the previous firmware right away. While on probation, ESP-IDF
 * refuses another update (esp_ota_begin()).
 *
 * Stacks: ESP-IDF's OTA functions map otadata into memory, which freezes
 * the PSRAM cache, so whoever calls them needs a stack in internal RAM
 * (otherwise an assert in esp_cache_freeze_caches_disable_interrupts(),
 * found 2026-11-15). The waiting happens in a PSRAM task; the confirmation
 * gets a short task with a small internal stack. Going back needs no OTA
 * call: a restart while on probation is what makes the bootloader go back,
 * and esp_restart() moves a PSRAM stack to internal RAM by itself
 * (FREERTOS_TASK_CREATE_ALLOW_EXT_MEM).
 */
#include "ota_guard.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"
#include "settings.h"
#include "weather_http.h"
#include "wifi_mgr.h"

static const char *TAG = "ota_guard";

#define CONFIRM_AFTER_S  120    /* the earliest confirmation */
#define GIVE_UP_AFTER_S  300    /* not confirmed by then: the previous firmware */
#define CHECK_EVERY_MS   5000
#define TASK_STACK       4096   /* the waiting, in PSRAM */
#define CONFIRM_STACK    3072   /* the confirmation, internal RAM, for a moment */
#define TASK_PRIORITY    2

/* Bench test of the rollback only; 0 in every build that stays. It acts only
 * while on probation, i.e. after an OTA upload:
 *   1 = crash 30 s after the start (the bootloader must go back)
 *   2 = act as if Wi-Fi never connected (back after 5 minutes) */
#ifndef OTA_GUARD_TEST
#define OTA_GUARD_TEST   0
#endif

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
static void confirm_task(void *arg)
{
    int up_s = (int)(intptr_t)arg;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "new firmware confirmed after %d s (Wi-Fi and web panel up)", up_s);
    } else {
        ESP_LOGE(TAG, "confirming the new firmware failed: %s", esp_err_to_name(err));
    }
    vTaskDeleteWithCaps(NULL);
}

static void probation_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_EVERY_MS));
        int up_s = (int)(esp_timer_get_time() / 1000000);
        bool wifi = wifi_mgr_is_connected();
        bool web = weather_http_server_up();
#if OTA_GUARD_TEST == 1
        if (up_s >= 30) {
            ESP_LOGE(TAG, "test: crashing on purpose");
            abort();
        }
#elif OTA_GUARD_TEST == 2
        wifi = false;
#endif
        if (up_s >= CONFIRM_AFTER_S && wifi && web) {
            if (xTaskCreateWithCaps(confirm_task, "ota_confirm", CONFIRM_STACK, (void *)(intptr_t)up_s,
                                    TASK_PRIORITY, NULL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS) {
                break;
            }
            /* tried again every few seconds, until the 5 minutes are up */
            ESP_LOGW(TAG, "no internal RAM for the confirmation yet");
        }
        if (up_s >= GIVE_UP_AFTER_S) {
            ESP_LOGE(TAG, "new firmware not confirmed within %d s (Wi-Fi %s, web panel %s): "
                     "back to the previous one", GIVE_UP_AFTER_S,
                     wifi ? "up" : "down", web ? "up" : "down");
            esp_restart();   /* still on probation: the bootloader goes back */
        }
    }
    vTaskDeleteWithCaps(NULL);   /* frees the PSRAM stack too */
}
#endif

void ota_guard_start(void)
{
#if CONFIG_SECURE_SIGNED_ON_UPDATE
    settings_get()->ota_enabled = true;
    ESP_LOGI(TAG, "signed build: updates allowed, only with our signature");
#else
    ESP_LOGW(TAG, "unsigned build: updates only once armed on the touchscreen, as upstream has it");
#endif

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;   /* not just updated: nothing to check */
    }
#if OTA_GUARD_TEST != 0
    ESP_LOGW(TAG, "test build %d: this firmware is meant to fail its probation", OTA_GUARD_TEST);
#endif
    ESP_LOGW(TAG, "new firmware in %s on probation: confirmed after %d s with Wi-Fi and the "
             "web panel up, otherwise back to the previous one after %d s",
             running->label, CONFIRM_AFTER_S, GIVE_UP_AFTER_S);
    if (xTaskCreateWithCaps(probation_task, "ota_guard", TASK_STACK, NULL, TASK_PRIORITY, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        /* without the check nothing would confirm it; a firmware that can't
           get a small task this early isn't one to keep */
        ESP_LOGE(TAG, "no task for the check: back to the previous firmware");
        esp_restart();
    }
#endif
}

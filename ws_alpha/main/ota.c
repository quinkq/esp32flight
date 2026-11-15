/*
 * Firmware updates over the air, the outdoor unit's side (PROTOCOL.md, "OTA
 * command"; the display's side is outdoor-link's outdoor_ota.c).
 *
 *  - The display offers an image in its replies: its address, the size and
 *    the SHA-256. The update runs after the exchange, with the battery at
 *    3.7 V or more: Wi-Fi (the networks in the secrets image, on the
 *    display's channel), the image over plain HTTP into the other app slot
 *    while hashing it. Size and hash must match the offer, and
 *    esp_ota_end() checks our signature (sdkconfig.defaults), so only our
 *    own firmware goes on, whoever sends the offer. Then a restart into it.
 *  - At most 3 attempts per image (told apart by its hash), and at most one
 *    an hour whatever is offered. An attempt is counted in NVS before it
 *    starts, so a brownout or a crash during it counts too. After the 3rd,
 *    the frames say WS_OUTDOOR_OTA_FAILED until the display stops offering
 *    that image; then its attempts are forgotten.
 *  - A new firmware's first wake is its probation: it must get a reply from
 *    the display (it keeps trying for up to 30 s, see main.c), or it goes
 *    back to the previous one. The bootloader can't decide that here: this
 *    build skips the boot checks after deep sleep
 *    (BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP), so a wake from deep sleep
 *    would start the new firmware again, still on probation.
 */
#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "config.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "link.h"
#include "mbedtls/sha256.h"
#include "power.h"
#include "sdkconfig.h"

static const char *TAG = "ota";

#define MIN_MV          3700           /* no update below this battery voltage */
#define MAX_TRIES       3              /* per image */
#define CAP_MS          90000          /* the whole update wake, at most */
#define WIFI_WAIT_MS    15000          /* per network */
#define HTTP_TIMEOUT_MS 10000
#define CHUNK           4096

/* Bench tests only; 0 in every build that goes to the outdoor unit.
 *   1 = the probation fails as if no reply came: goes back at once
 *   2 = a wrong Wi-Fi password, and attempts 3 minutes apart */
#ifndef OTA_TEST
#define OTA_TEST        0
#endif

#if OTA_TEST == 2
#define RETRY_S         180
#else
#define RETRY_S         3600           /* at least this between attempts */
#endif

#define BIT_GOT_IP      BIT0
#define BIT_FAILED      BIT1

static bool s_loaded;                  /* s_rec read from NVS this wake */
static config_ota_t s_rec;
static bool s_offered;                 /* this wake's replies offer an image */
static ws_link_reply_t s_offer;
static EventGroupHandle_t s_wifi_ev;

static void load(void)
{
    if (!s_loaded) {
        config_load_ota(&s_rec);
        s_loaded = true;
    }
}

bool ota_on_probation(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    return running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

void ota_end_probation(bool replied)
{
#if OTA_TEST == 1
    ESP_LOGW(TAG, "test build 1: failing the probation on purpose");
    replied = false;
#endif
    if (replied) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGW(TAG, "new firmware confirmed: the display answered");
        } else {
            ESP_LOGE(TAG, "confirming the new firmware failed");
        }
        config_erase_ota();   /* its attempts are over */
        s_loaded = false;
        return;
    }
    ESP_LOGE(TAG, "new firmware got no answer from the display: back to the previous one");
    power_count_awake();
    esp_ota_mark_app_invalid_rollback_and_reboot();
    /* returns only when there's nothing to go back to */
    ESP_LOGE(TAG, "no previous firmware to go back to: keeping this one");
    esp_ota_mark_app_valid_cancel_rollback();
}

void ota_note_reply(const ws_link_reply_t *r)
{
    load();
    if (r->cmd != WS_CMD_OTA) {
        s_offered = false;
        if (s_rec.tries != 0) {   /* no longer offered: forget its attempts, keep the hour */
            memset(s_rec.sha8, 0, sizeof(s_rec.sha8));
            s_rec.tries = 0;
            config_save_ota(&s_rec);
        }
        return;
    }
    memcpy(&s_offer, r, sizeof(s_offer));
    s_offered = true;
    if (memcmp(s_rec.sha8, r->ota_sha256, sizeof(s_rec.sha8)) != 0) {
        /* another image: attempts of its own. Stored now, so the next
           frames don't report a give-up that belonged to the old one. */
        memcpy(s_rec.sha8, r->ota_sha256, sizeof(s_rec.sha8));
        s_rec.tries = 0;
        config_save_ota(&s_rec);
    }
}

bool ota_gave_up(void)
{
    load();
    return s_rec.tries >= MAX_TRIES;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_ev, BIT_GOT_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_ev, BIT_FAILED);
    }
}

/* Wi-Fi up with an address, on one of the two stored networks */
static bool wifi_up(void)
{
    s_wifi_ev = xEventGroupCreate();
    esp_wifi_deinit();   /* link_stop() left it initialised for ESP-NOW */
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_event_loop_create_default();
    if (s_wifi_ev == NULL || (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ||
        esp_netif_init() != ESP_OK || esp_netif_create_default_wifi_sta() == NULL ||
        esp_wifi_init(&init) != ESP_OK ||
        esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
        esp_wifi_set_country_code("PL", false) != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL) != ESP_OK ||
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi, NULL) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi setup failed");
        return false;
    }
    esp_wifi_set_max_tx_power(CONFIG_WS_ALPHA_TX_POWER_DBM * 4);

    for (int n = 1; n <= 2; n++) {
        config_wifi_t w;
        if (!config_load_wifi(n, &w)) {
            continue;
        }
#if OTA_TEST == 2
        strlcpy(w.pass, "wrong-on-purpose", sizeof(w.pass));
#endif
        wifi_config_t cfg = { 0 };
        strlcpy((char *)cfg.sta.ssid, w.ssid, sizeof(cfg.sta.ssid));
        strlcpy((char *)cfg.sta.password, w.pass, sizeof(cfg.sta.password));
        cfg.sta.channel = link_channel();   /* the display's: its access point's */
        memset(&w, 0, sizeof(w));
        xEventGroupClearBits(s_wifi_ev, BIT_GOT_IP | BIT_FAILED);
        if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK || esp_wifi_connect() != ESP_OK) {
            continue;
        }
        const EventBits_t bits = xEventGroupWaitBits(s_wifi_ev, BIT_GOT_IP | BIT_FAILED, pdFALSE, pdFALSE,
                                                     pdMS_TO_TICKS(WIFI_WAIT_MS));
        if (bits & BIT_GOT_IP) {
            ESP_LOGI(TAG, "Wi-Fi network %d connected", n);
            return true;
        }
        ESP_LOGW(TAG, "Wi-Fi network %d didn't connect", n);
        esp_wifi_disconnect();
    }
    return false;
}

static void wifi_down(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
}

/* The image into the other app slot; NULL when it's written and set to
 * start, else why not. */
static const char *download(void)
{
    static char why[64];
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return "no app slot to write to";
    }
    if (!wifi_up()) {
        return "Wi-Fi didn't connect";
    }
    char url[48];
    snprintf(url, sizeof(url), "http://%u.%u.%u.%u" WS_LINK_OTA_PATH,
             s_offer.ota_ip[0], s_offer.ota_ip[1], s_offer.ota_ip[2], s_offer.ota_ip[3]);
    const esp_http_client_config_t hc = { .url = url, .timeout_ms = HTTP_TIMEOUT_MS };
    esp_http_client_handle_t client = esp_http_client_init(&hc);
    uint8_t *buf = malloc(CHUNK);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    esp_ota_handle_t ota = 0;
    bool begun = false;
    const char *err = NULL;
    uint32_t got = 0;

    if (client == NULL || buf == NULL) {
        err = "no memory";
    } else if (esp_http_client_open(client, 0) != ESP_OK || esp_http_client_fetch_headers(client) < 0) {
        err = "the display didn't answer";
    } else if (esp_http_client_get_status_code(client) != 200) {
        snprintf(why, sizeof(why), "the display answered HTTP %d", esp_http_client_get_status_code(client));
        err = why;
    } else if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        err = "can't write the app slot";
    } else {
        begun = true;
        for (;;) {
            const int n = esp_http_client_read(client, (char *)buf, CHUNK);
            if (n < 0) {
                err = "the download broke off";
                break;
            }
            if (n == 0) {
                break;   /* the end (the display sends it chunked) */
            }
            if ((uint32_t)n > s_offer.ota_size - got) {
                err = "more data than offered";
                break;
            }
            mbedtls_sha256_update(&sha, buf, n);
            if (esp_ota_write(ota, buf, n) != ESP_OK) {
                err = "writing the app slot failed";
                break;
            }
            got += n;
        }
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (err == NULL && got != s_offer.ota_size) {
        snprintf(why, sizeof(why), "got %lu of %lu bytes", (unsigned long)got, (unsigned long)s_offer.ota_size);
        err = why;
    }
    if (err == NULL && memcmp(digest, s_offer.ota_sha256, sizeof(digest)) != 0) {
        err = "the hash doesn't match the offer";
    }
    if (err == NULL) {
        begun = false;
        const esp_err_t e = esp_ota_end(ota);   /* checks the image and our signature */
        if (e != ESP_OK) {
            err = e == ESP_ERR_OTA_VALIDATE_FAILED ? "image refused: not signed with our key, or broken"
                                                   : "finishing the app slot failed";
        } else if (esp_ota_set_boot_partition(part) != ESP_OK) {
            err = "can't set the new firmware to start";
        }
    }
    if (begun) {
        esp_ota_abort(ota);
    }
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    free(buf);
    return err;
}

void ota_run(uint16_t bat_mv)
{
    if (!s_offered) {
        return;
    }
    load();
    if (bat_mv == WS_LINK_NO_U16 || bat_mv < MIN_MV) {
        ESP_LOGW(TAG, "an update is offered; waiting for the battery (%u mV, needs %d)", bat_mv, MIN_MV);
        return;
    }
    if (s_rec.tries >= MAX_TRIES) {
        ESP_LOGW(TAG, "an update is offered; gave up on it after %d attempts", MAX_TRIES);
        return;
    }
    const uint32_t now = (uint32_t)time(NULL);
    if (s_rec.last_try != 0 && now >= s_rec.last_try && now - s_rec.last_try < RETRY_S) {
        ESP_LOGW(TAG, "an update is offered; next attempt in %lu min",
                 (unsigned long)((RETRY_S - (now - s_rec.last_try) + 59) / 60));
        return;
    }
    if (s_offer.ota_size == 0 || s_offer.ota_size > WS_LINK_OTA_MAX_SIZE) {
        ESP_LOGW(TAG, "an update is offered with a size of %lu bytes: ignored", (unsigned long)s_offer.ota_size);
        return;
    }

    s_rec.tries++;
    s_rec.last_try = now;
    config_save_ota(&s_rec);   /* before anything can go wrong */
    power_extend_cap(CAP_MS);
    ESP_LOGW(TAG, "update: attempt %u of %d, %lu bytes from %u.%u.%u.%u", s_rec.tries, MAX_TRIES,
             (unsigned long)s_offer.ota_size, s_offer.ota_ip[0], s_offer.ota_ip[1], s_offer.ota_ip[2],
             s_offer.ota_ip[3]);
    const char *err = download();
    if (err == NULL) {
        ESP_LOGW(TAG, "update written: restarting into the new firmware");
        wifi_down();
        power_count_awake();   /* the restart skips power_sleep() */
        esp_restart();
    }
    ESP_LOGE(TAG, "update failed: %s", err);
    wifi_down();
}

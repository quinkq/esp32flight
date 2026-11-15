/*
 * Firmware updates for the outdoor unit, the display's side (weather-station
 * fork). Flow in PROTOCOL.md ("OTA command"):
 *
 *  - The image is uploaded on the weather station page (main/weather_http.c)
 *    and kept in the display's spare app slot (decided 2026-11-15; PSRAM had
 *    no room for the 1 MB image). That slot holds the display's previous
 *    firmware, which matters only while a new display firmware is on
 *    probation (main/ota_guard.c), so:
 *      - an upload is refused while the display itself is on probation;
 *      - before the first byte goes in, the slot is struck from otadata, so
 *        the bootloader can never try to start the outdoor unit's image;
 *      - a display update later writes the slot and otadata as usual, and
 *        a waiting outdoor image is simply gone (noticed below).
 *  - A small marker in NVS (namespace "ws_ota") says what waits; at boot the
 *    slot is checked against it (header and SHA-256), so a waiting image
 *    survives a display restart.
 *  - Before it's offered: an ESP-IDF image for the ESP32-C3, the app
 *    "ws_alpha", signed (a signature sector at the end), and a build number
 *    other than the one the outdoor unit runs (the display tells the builds
 *    apart by it).
 *  - While it waits, every reply offers it (WS_CMD_OTA: our address, size,
 *    SHA-256), once the slot's first bytes still match. The outdoor unit
 *    downloads it over Wi-Fi from WS_LINK_OTA_PATH, checks hash and
 *    signature, and restarts into it.
 *  - A frame from the new build ends the offer ("started"); a later frame
 *    from it after a deep-sleep wake means the new firmware confirmed itself
 *    ("done"). A frame from the old build after "started" means it went
 *    back. The outdoor unit gives up on an image after 3 attempts at least an
 *    hour apart and says so (WS_OUTDOOR_OTA_FAILED); that flag counts only
 *    once it has heard our offer, so a give-up left over from an earlier
 *    image can't fail a new one.
 *
 * Tasks: the link task (frames, offers: small flash reads, which are fine on
 * its PSRAM stack), the web server and app_main (upload, otadata, the boot
 * check: internal stacks, which ESP-IDF's OTA functions need), the Wi-Fi task
 * (offer_heard: an atomic counter only).
 */
#include "outdoor_ota.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "outdoor_link.h"

static const char *TAG = "outdoor_ota";

#define BLOCK        (64 * 1024)      /* erased ahead of the writes */
#define HEAD         256              /* the image's first bytes: checks, and "still ours?" */
#define READ_CHUNK   4096
#define PROJECT      "ws_alpha"
#define SIG_MAGIC    0xE7             /* a secure boot v2 signature block */
#define SIG_VERSION  0x02
#define DEEP_SLEEP_WAKE 8             /* ESP_RST_DEEPSLEEP */
#define NVS_NS       "ws_ota"
#define NVS_KEY      "img"
#define MARK_MAGIC   0x57534f31u      /* "WSO1" */

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint16_t build, from_build;
    uint8_t  sha[32];
} marker_t;

static SemaphoreHandle_t s_mux;
static const esp_partition_t *s_part; /* the display's spare app slot */
static uint32_t s_size, s_received, s_erased;
static bool s_uploading;
static mbedtls_sha256_context s_sha_ctx;
static uint8_t s_sha[32];
static uint8_t s_head[HEAD];
static outdoor_ota_info_t s_info;     /* state, builds, counters, why */
static volatile bool s_reply_offered; /* the reply being sent carries the offer */
static uint32_t s_heard;              /* offer replies acknowledged: atomic */

static void set_state(outdoor_ota_state_t st)
{
    s_info.state = st;
    s_info.changed_us = esp_timer_get_time();
}

static void event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void event(const char *fmt, ...)
{
    char text[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    outdoor_link_event("update", text);
}

static void marker_save(void)
{
    marker_t m = {
        .magic = MARK_MAGIC, .size = s_size, .build = s_info.build, .from_build = s_info.from_build,
    };
    memcpy(m.sha, s_sha, sizeof(m.sha));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_blob(h, NVS_KEY, &m, sizeof(m)) != ESP_OK || nvs_commit(h) != ESP_OK) {
            ESP_LOGW(TAG, "marker not saved: a display restart forgets the waiting image");
        }
        nvs_close(h);
    }
}

static void marker_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_erase_key(h, NVS_KEY) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
}

/* the image's checks; head = its first HEAD bytes. NULL if it may go out. */
static const char *check_image(const uint8_t *head, uint32_t size, uint16_t *build)
{
    const esp_image_header_t *h = (const esp_image_header_t *)head;
    if (h->magic != ESP_IMAGE_HEADER_MAGIC) {
        return "not an ESP-IDF firmware image";
    }
    if (h->chip_id != ESP_CHIP_ID_ESP32C3) {
        return "built for another chip, not the outdoor unit's ESP32-C3";
    }
    const esp_app_desc_t *d = (const esp_app_desc_t *)(head + sizeof(esp_image_header_t) +
                                                       sizeof(esp_image_segment_header_t));
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strncmp(d->project_name, PROJECT, sizeof(d->project_name)) != 0) {
        return "not the outdoor unit's firmware (" PROJECT ")";
    }
    const int b = atoi(d->version);
    if (b <= 0 || b > UINT16_MAX) {
        return "no build number in the image";
    }
    uint8_t sig[2] = { 0 };
    if (size % 4096 != 0 || esp_partition_read(s_part, size - 4096, sig, sizeof(sig)) != ESP_OK ||
        sig[0] != SIG_MAGIC || sig[1] != SIG_VERSION) {
        return "not signed: build it with the signing key (ws_alpha/sdkconfig.defaults)";
    }
    *build = (uint16_t)b;
    return NULL;
}

/* A marker from before the restart, and the slot still holds that image:
 * offered again. Runs in app_main (outdoor_link_start()). */
static void restore(void)
{
    marker_t m;
    size_t len = sizeof(m);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    const bool have = nvs_get_blob(h, NVS_KEY, &m, &len) == ESP_OK && len == sizeof(m) && m.magic == MARK_MAGIC;
    nvs_close(h);
    if (!have) {
        return;
    }
    uint8_t *buf = heap_caps_malloc(READ_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = buf != NULL && s_part != NULL && m.size <= s_part->size && m.size >= READ_CHUNK;
    uint16_t build = 0;
    if (ok) {
        mbedtls_sha256_context sha;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);
        for (uint32_t off = 0; ok && off < m.size; off += READ_CHUNK) {
            const uint32_t n = m.size - off < READ_CHUNK ? m.size - off : READ_CHUNK;
            ok = esp_partition_read(s_part, off, buf, n) == ESP_OK;
            if (ok && off == 0) {
                memcpy(s_head, buf, HEAD);
            }
            mbedtls_sha256_update(&sha, buf, n);
        }
        uint8_t digest[32];
        mbedtls_sha256_finish(&sha, digest);
        mbedtls_sha256_free(&sha);
        ok = ok && memcmp(digest, m.sha, sizeof(digest)) == 0 &&
             check_image(s_head, m.size, &build) == NULL && build == m.build;
    }
    free(buf);
    if (!ok) {
        ESP_LOGI(TAG, "the outdoor unit image from before the restart is gone (a display update?)");
        marker_erase();
        return;
    }
    s_size = m.size;
    memcpy(s_sha, m.sha, sizeof(s_sha));
    s_info.build = m.build;
    s_info.from_build = m.from_build;
    s_info.size = m.size;
    set_state(OUTDOOR_OTA_WAITING);
    ESP_LOGI(TAG, "build %u still waits for the outdoor unit (%lu KB in %s)",
             m.build, (unsigned long)(m.size / 1024), s_part->label);
}

bool outdoor_ota_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    if (s_mux == NULL) {
        return false;
    }
    s_part = esp_ota_get_next_update_partition(NULL);
    restore();
    return true;
}

/* ---- upload (web server) ---- */

static bool display_on_probation(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    return running != NULL && esp_ota_get_state_partition(running, &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

const char *outdoor_ota_begin(size_t size)
{
    if (s_mux == NULL) {
        return "the link to the outdoor unit is off";
    }
    if (s_part == NULL) {
        return "this display has no spare app slot to keep the image in";
    }
    if (size > WS_LINK_OTA_MAX_SIZE || size > s_part->size) {
        return "too big for the outdoor unit (1.5 MB at most): the display's own firmware?";
    }
    if (size < 4096) {
        return "too small to be an outdoor unit image";
    }
    /* its spare slot is the way back for a display update still on probation */
    if (display_on_probation()) {
        return "the display was just updated itself and is still checking it: try again in 5 minutes";
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    const bool replacing = s_info.state == OUTDOOR_OTA_WAITING;
    const uint16_t old = s_info.build;
    set_state(OUTDOOR_OTA_NONE);
    xSemaphoreGive(s_mux);
    marker_erase();
    if (replacing) {
        event("build %u replaced by a new upload", old);
    }
    /* never bootable from now on, whatever ends up in the slot. ESP_FAIL:
       no otadata entry points at it (after erase-otadata, say): nothing to
       strike. Anything else is a flash error. */
    const esp_err_t err = esp_ota_invalidate_inactive_ota_data_slot();
    if (err != ESP_OK && err != ESP_FAIL) {
        ESP_LOGW(TAG, "striking %s from otadata: %s", s_part->label, esp_err_to_name(err));
        return "can't reserve the display's spare app slot";
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_size = (uint32_t)size;
    s_received = s_erased = 0;
    s_uploading = true;
    mbedtls_sha256_init(&s_sha_ctx);
    mbedtls_sha256_starts(&s_sha_ctx, 0);
    xSemaphoreGive(s_mux);
    ESP_LOGI(TAG, "outdoor unit image, %u bytes -> %s", (unsigned)size, s_part->label);
    return NULL;
}

const char *outdoor_ota_write(const void *data, size_t len)
{
    if (s_mux == NULL || !s_uploading) {
        return "no upload in progress";
    }
    if (len > s_size - s_received) {
        return "more data than announced";
    }
    const char *why = NULL;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    while (why == NULL && s_erased < s_received + len) {
        if (esp_partition_erase_range(s_part, s_erased, BLOCK) != ESP_OK) {
            why = "erasing the display's spare app slot failed";
        }
        s_erased += BLOCK;
    }
    if (why == NULL && esp_partition_write(s_part, s_received, data, len) != ESP_OK) {
        why = "writing the display's spare app slot failed";
    }
    if (why == NULL) {
        if (s_received < HEAD) {
            const size_t n = len < HEAD - s_received ? len : HEAD - s_received;
            memcpy(s_head + s_received, data, n);
        }
        mbedtls_sha256_update(&s_sha_ctx, data, len);
        s_received += len;
    }
    xSemaphoreGive(s_mux);
    return why;
}

const char *outdoor_ota_end(void)
{
    if (s_mux == NULL || !s_uploading) {
        return "no upload in progress";
    }
    outdoor_health_t h;
    outdoor_link_health(&h);

    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_uploading = false;
    mbedtls_sha256_finish(&s_sha_ctx, s_sha);
    mbedtls_sha256_free(&s_sha_ctx);
    static char why[128];   /* the web server is one task: one upload at a time */
    uint16_t build = 0;
    const char *bad = s_received != s_size ? "the upload is incomplete" : check_image(s_head, s_size, &build);
    if (bad == NULL && h.last_heard_us != 0 && h.fw_build == build) {
        snprintf(why, sizeof(why), "the outdoor unit runs build %u already: raise PROJECT_VER in ws_alpha/CMakeLists.txt",
                 build);
        bad = why;
    }
    if (bad != NULL) {
        xSemaphoreGive(s_mux);
        ESP_LOGW(TAG, "upload refused: %s", bad);
        return bad;
    }
    memset(&s_info, 0, sizeof(s_info));
    s_info.build = build;
    s_info.from_build = h.last_heard_us != 0 ? h.fw_build : 0;
    s_info.size = s_size;
    __atomic_store_n(&s_heard, 0, __ATOMIC_RELAXED);
    set_state(OUTDOOR_OTA_WAITING);
    xSemaphoreGive(s_mux);
    marker_save();
    event("build %u uploaded (%lu KB): offered to the outdoor unit from its next wake",
          build, (unsigned long)(s_info.size / 1024));
    return NULL;
}

void outdoor_ota_abort(void)
{
    if (s_mux == NULL) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_uploading) {
        s_uploading = false;
        mbedtls_sha256_free(&s_sha_ctx);
    }
    xSemaphoreGive(s_mux);
}

bool outdoor_ota_cancel(void)
{
    if (s_mux == NULL) {
        return false;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    const bool waiting = s_info.state == OUTDOOR_OTA_WAITING;
    if (waiting) {
        set_state(OUTDOOR_OTA_CANCELLED);
    }
    xSemaphoreGive(s_mux);
    if (waiting) {
        marker_erase();
        event("build %u cancelled", s_info.build);
    }
    return waiting;
}

void outdoor_ota_info(outdoor_ota_info_t *out)
{
    if (s_mux == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_info;
    xSemaphoreGive(s_mux);
    out->offers_heard = (uint16_t)__atomic_load_n(&s_heard, __ATOMIC_RELAXED);
}

/* ---- download by the outdoor unit (web server) ---- */

bool outdoor_ota_download_begin(uint32_t *size)
{
    if (s_mux == NULL) {
        return false;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    const bool waiting = s_info.state == OUTDOOR_OTA_WAITING;
    if (waiting) {
        s_info.downloads++;
        *size = s_size;
    }
    xSemaphoreGive(s_mux);
    return waiting;
}

size_t outdoor_ota_download_read(uint32_t offset, void *buf, size_t len)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    size_t n = 0;
    if (s_info.state == OUTDOOR_OTA_WAITING && offset < s_size) {
        n = len < s_size - offset ? len : s_size - offset;
        if (esp_partition_read(s_part, offset, buf, n) != ESP_OK) {
            n = 0;
        }
    }
    xSemaphoreGive(s_mux);
    return n;
}

void outdoor_ota_download_end(bool complete, uint32_t sent)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (complete) {
        s_info.downloads_complete++;
    }
    const uint16_t build = s_info.build;
    xSemaphoreGive(s_mux);
    if (complete) {
        event("the outdoor unit downloaded build %u", build);
    } else {
        event("a download of build %u broke off after %lu KB", build, (unsigned long)(sent / 1024));
    }
}

/* ---- the link task ---- */

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* with s_mux held; the caller erases the marker after releasing it */
static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_info.why, sizeof(s_info.why), fmt, ap);
    va_end(ap);
    set_state(OUTDOOR_OTA_FAILED);
}

void outdoor_ota_frame(uint16_t fw_build, bool gave_up, uint8_t reset_reason)
{
    if (s_mux == NULL) {
        return;
    }
    char text[200] = "";
    bool forget = false;   /* the marker: nothing waits any more */
    xSemaphoreTake(s_mux, portMAX_DELAY);
    const uint16_t build = s_info.build;
    switch (s_info.state) {
    case OUTDOOR_OTA_WAITING:
        if (s_info.from_build == 0) {
            s_info.from_build = fw_build;
        }
        if (fw_build == build) {
            set_state(OUTDOOR_OTA_STARTED);
            forget = true;
            snprintf(text, sizeof(text), "build %u started on the outdoor unit (was %u); "
                     "done once it wakes from deep sleep on it", build, s_info.from_build);
        } else if (gave_up && __atomic_load_n(&s_heard, __ATOMIC_RELAXED) > 0) {
            if (s_info.downloads == 0) {
                fail("it gave up after 3 attempts without downloading it: its Wi-Fi didn't connect "
                     "(are both networks and passwords right in its secrets?)");
            } else if (s_info.downloads_complete == 0) {
                fail("it gave up after 3 attempts: every download broke off");
            } else {
                fail("it downloaded it %u times and refused it: a wrong signature or a broken image "
                     "(its serial log says which)", s_info.downloads_complete);
            }
            forget = true;
            snprintf(text, sizeof(text), "build %u failed: %s", build, s_info.why);
        }
        break;
    case OUTDOOR_OTA_STARTED:
        if (fw_build != build) {
            fail("build %u started, but the outdoor unit went back to build %u: the new firmware "
                 "didn't get an answer in its first wake, or crashed", build, fw_build);
            snprintf(text, sizeof(text), "build %u failed: %s", build, s_info.why);
        } else if (reset_reason == DEEP_SLEEP_WAKE) {
            set_state(OUTDOOR_OTA_DONE);
            snprintf(text, sizeof(text), "build %u kept: update done", build);
        }
        break;
    default:
        break;
    }
    xSemaphoreGive(s_mux);
    if (forget) {
        marker_erase();
    }
    if (text[0] != '\0') {
        outdoor_link_event("update", text);
    }
}

/* the slot still starts with the waiting image: a display update would
 * have written its own there */
static bool still_ours(void)
{
    uint8_t now[64];
    return esp_partition_read(s_part, 0, now, sizeof(now)) == ESP_OK && memcmp(now, s_head, sizeof(now)) == 0;
}

bool outdoor_ota_offer(ws_link_reply_t *r)
{
    s_reply_offered = false;
    if (s_mux == NULL) {
        return false;
    }
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return false;   /* nowhere to download from */
    }
    bool gone = false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool waiting = s_info.state == OUTDOOR_OTA_WAITING;
    if (waiting && !s_uploading && !still_ours()) {
        fail("overwritten by a display update before the outdoor unit took it");
        waiting = false;
        gone = true;
    }
    if (waiting) {
        r->cmd = WS_CMD_OTA;
        memcpy(r->ota_ip, &ip.ip.addr, sizeof(r->ota_ip));   /* a.b.c.d in memory order */
        r->ota_size = s_size;
        memcpy(r->ota_sha256, s_sha, sizeof(r->ota_sha256));
    }
    const uint16_t build = s_info.build;
    xSemaphoreGive(s_mux);
    if (gone) {
        marker_erase();
        event("build %u failed: overwritten by a display update before the outdoor unit took it", build);
    }
    s_reply_offered = waiting;
    return waiting;
}

void outdoor_ota_offer_heard(void)
{
    if (s_reply_offered) {
        __atomic_add_fetch(&s_heard, 1, __ATOMIC_RELAXED);
    }
}

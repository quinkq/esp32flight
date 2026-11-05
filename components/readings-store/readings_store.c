/*
 * The readings store (readings_store.h; "Storage" in DISPLAY.md, in the
 * planning notes).
 *
 *  - RAM holds the whole ring: slot n at n % READINGS_SLOTS, and a bit for
 *    each record flash doesn't have yet. Every reading goes there first.
 *  - Flash: the "readings" partition, PHYS sectors of 4 KB, no file system
 *    (SPIFFS in the 94 %-full /assets spent half a minute in garbage
 *    collection for one file, 2026-11-04). Each sector holds one part of the
 *    ring (PER_SECTOR records) under a 16-byte header: which part, a sequence
 *    number, a CRC-32 of the records. One sector is always spare.
 *  - A part is rewritten whole: into a blank sector, its header last, and
 *    only then is the old copy erased. A power loss leaves the old copy, or
 *    both (the higher sequence number wins at boot), never neither.
 *  - Wear: one sector erase per flush (a burst of frames), ~96 a day at the
 *    15-minute interval. Taking the next blank sector in turn spreads them,
 *    ~2,500 a year per sector against the flash's 100,000.
 *  - The caches stay on during flash writes in this build (code and rodata
 *    run from PSRAM), so other tasks and the 7B's panel don't wait.
 *  - A record counts only when its ts belongs to its position and its
 *    checksum fits.
 *  - A reading belongs to the slot whose start is nearest: the outdoor unit
 *    wakes on the slot boundaries by its own clock, which runs a little fast
 *    or slow, so its readings fall a few seconds either side of them.
 *  - Several readings in one slot (intervals of 1 or 5 minutes) give means,
 *    each value over the readings that have it. The sums for the slot being
 *    filled stay in RAM; after a display restart they're picked up from the
 *    record.
 *  - Each change to a slot gets the next change number (RAM only), so a
 *    client asks for what changed since its last visit: late readings from
 *    the backlog and slots still filling included. The numbers restart at a
 *    boot, under a new random boot id; a client's older token then gets
 *    everything once more.
 *  - Two locks: s_mux for RAM, held briefly (the UI will read here), and
 *    s_flash around flash access and the sector map.
 */
#include "readings_store.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "readings";

#define SECTOR       4096
#define PHYS         14                 /* sectors of the partition in use */
#define PARTS        (PHYS - 1)         /* parts of the ring; one sector is spare */
#define HEADER_SIZE  16
#define PER_SECTOR   ((SECTOR - HEADER_SIZE) / sizeof(readings_slot_t))  /* 170 */
#define SECTOR_MAGIC 0x31525357u        /* "WSR1" in flash */
#define FORMAT       1
#define BATCH        16                 /* slots copied per hold of s_mux for a reader */

typedef struct {
    uint32_t magic;
    uint16_t part;                      /* which part of the ring: 0..PARTS-1 */
    uint8_t  format;
    uint8_t  record_size;
    uint32_t seq;                       /* higher: the newer copy */
    uint32_t crc;                       /* CRC-32 of the records */
} sector_hdr_t;

_Static_assert(sizeof(readings_slot_t) == 24, "a slot is 24 bytes in flash");
_Static_assert(offsetof(readings_slot_t, check) == sizeof(readings_slot_t) - 1, "check is the last byte");
_Static_assert(sizeof(sector_hdr_t) == HEADER_SIZE, "a sector's header is 16 bytes");
_Static_assert(HEADER_SIZE + PER_SECTOR * sizeof(readings_slot_t) == SECTOR, "the records fill a sector");
_Static_assert(READINGS_SLOTS == PARTS * PER_SECTOR, "the ring fills the parts");
_Static_assert(READINGS_SHT_OK == WS_REC_SHT_OK && READINGS_BME_OK == WS_REC_BME_OK,
               "the sensor bits are ws-link's");

/* sums for the slot being filled */
typedef struct {
    uint32_t slot;                      /* 0: none */
    int32_t  sht_t, sht_rh, bme_t, bme_rh, bme_p, bat;
    int32_t  n_sht, n_bme, n_bat;
} sums_t;

typedef enum { LOAD_OK, LOAD_FOREIGN, LOAD_FAILED } load_t;

static SemaphoreHandle_t s_mux;         /* guards RAM, the state and s_failing */
static SemaphoreHandle_t s_flash;       /* around flash access and the sector map */
static readings_slot_t *s_ram;          /* READINGS_SLOTS, in PSRAM; NULL: not started */
static uint32_t s_unwritten[(READINGS_SLOTS + 31) / 32];  /* a bit per record flash doesn't have yet */
static uint32_t *s_changed;             /* each slot's last change number, in PSRAM; 1: loaded at boot */
static uint32_t s_change = 1;           /* the latest change number */
static uint32_t s_boot;                 /* random, for the cursor tokens */
static uint32_t s_newest;               /* newest slot; 0: none */
static sums_t s_sums;
static readings_store_state_t s_state = READINGS_OFF;
static char s_why[sizeof(((readings_store_info_t *)0)->why)];
static bool s_failing;

/* the sector map, under s_flash */
static const esp_partition_t *s_part;
static uint8_t *s_buf;                  /* one sector, in PSRAM */
static int8_t s_where[PARTS];           /* the sector holding each part; -1: none yet */
static int8_t s_owner[PHYS];            /* the part each sector holds; -1: free */
static bool s_blank[PHYS];              /* free and erased */
static uint32_t s_seq_of[PHYS];
static uint32_t s_seq;                  /* the newest copy's */
static int s_next;                      /* where the search for a blank sector starts */

static uint8_t check_of(const readings_slot_t *s)
{
    return esp_rom_crc8_le(0, (const uint8_t *)s, offsetof(readings_slot_t, check));
}

static void mark(uint32_t pos)
{
    s_unwritten[pos / 32] |= 1u << (pos % 32);
}

static void unmark(uint32_t pos)
{
    s_unwritten[pos / 32] &= ~(1u << (pos % 32));
}

static bool marked(uint32_t pos)
{
    return s_unwritten[pos / 32] & 1u << (pos % 32);
}

static void ram_only(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void ram_only(const char *fmt, ...)
{
    char why[sizeof(s_why)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "readings stay in RAM: %s", why);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state = READINGS_RAM_ONLY;
    strlcpy(s_why, why, sizeof(s_why));
    xSemaphoreGive(s_mux);
}

static readings_slot_t empty(uint32_t slot)
{
    return (readings_slot_t){
        .ts = slot * READINGS_SLOT_S,
        .sht_t = WS_LINK_NO_I16,
        .sht_rh = WS_LINK_NO_U16,
        .bme_t = WS_LINK_NO_I16,
        .bme_rh = WS_LINK_NO_U16,
        .bme_p = WS_LINK_NO_U16,
        .bat_mv = WS_LINK_NO_U16,
        .rssi = READINGS_NO_RSSI,
        .in_t = WS_LINK_NO_I16,
        .in_rh = WS_LINK_NO_U16,
    };
}

/* rounded to the nearest */
static int32_t mean(int32_t sum, int32_t n)
{
    return sum >= 0 ? (sum + n / 2) / n : -((-sum + n / 2) / n);
}

void readings_store_put_outdoor(const ws_link_record_t *r, uint32_t ts, int8_t rssi, uint8_t flags)
{
    const uint32_t slot = (ts + READINGS_SLOT_S / 2) / READINGS_SLOT_S;
    if (s_ram == NULL || slot == 0) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    const uint32_t pos = slot % READINGS_SLOTS;
    readings_slot_t *s = &s_ram[pos];
    if (s->ts > slot * READINGS_SLOT_S) {
        xSemaphoreGive(s_mux);
        ESP_LOGW(TAG, "reading at %lu dropped: older than the ring", (unsigned long)ts);
        return;
    }
    if (s_sums.slot != slot) {
        s_sums = (sums_t){ .slot = slot };
        if (s->ts == slot * READINGS_SLOT_S) {
            /* readings from before a display restart: carry on with their means */
            const int32_t n = s->count;
            if (s->flags & READINGS_SHT_OK) {
                s_sums.n_sht = n;
                s_sums.sht_t = s->sht_t * n;
                s_sums.sht_rh = s->sht_rh * n;
            }
            if (s->flags & READINGS_BME_OK) {
                s_sums.n_bme = n;
                s_sums.bme_t = s->bme_t * n;
                s_sums.bme_rh = s->bme_rh * n;
                s_sums.bme_p = s->bme_p * n;
            }
            if (s->bat_mv != WS_LINK_NO_U16) {
                s_sums.n_bat = n;
                s_sums.bat = s->bat_mv * n;
            }
        } else {
            *s = empty(slot);           /* replaces the one a ring's length older, if any */
        }
    }

    if (r->flags & WS_REC_SHT_OK) {
        s_sums.sht_t += r->sht_t;
        s_sums.sht_rh += r->sht_rh;
        s_sums.n_sht++;
        s->sht_t = (int16_t)mean(s_sums.sht_t, s_sums.n_sht);
        s->sht_rh = (uint16_t)mean(s_sums.sht_rh, s_sums.n_sht);
        s->flags |= READINGS_SHT_OK;
    }
    if (r->flags & WS_REC_BME_OK) {
        s_sums.bme_t += r->bme_t;
        s_sums.bme_rh += r->bme_rh;
        s_sums.bme_p += r->bme_p;
        s_sums.n_bme++;
        s->bme_t = (int16_t)mean(s_sums.bme_t, s_sums.n_bme);
        s->bme_rh = (uint16_t)mean(s_sums.bme_rh, s_sums.n_bme);
        s->bme_p = (uint16_t)mean(s_sums.bme_p, s_sums.n_bme);
        s->flags |= READINGS_BME_OK;
    }
    if (r->bat_mv != WS_LINK_NO_U16) {
        s_sums.bat += r->bat_mv;
        s_sums.n_bat++;
        s->bat_mv = (uint16_t)mean(s_sums.bat, s_sums.n_bat);
    }
    if (s->count < UINT8_MAX) {
        s->count++;
    }
    s->flags |= flags & (READINGS_LATE | READINGS_LOW_BATTERY);
    if (rssi != READINGS_NO_RSSI) {
        s->rssi = rssi;
    }
    s->check = check_of(s);
    mark(pos);
    s_changed[pos] = ++s_change;
    if (slot > s_newest) {
        s_newest = slot;
    }
    xSemaphoreGive(s_mux);
}

static bool erase(int p)
{
    s_blank[p] = esp_partition_erase_range(s_part, (size_t)p * SECTOR, SECTOR) == ESP_OK;
    return s_blank[p];
}

/* A blank sector to write to, the next one in turn; -1: none. */
static int blank_sector(void)
{
    for (int i = 0; i < PHYS; i++) {
        const int p = (s_next + i) % PHYS;
        if (s_owner[p] < 0 && (s_blank[p] || erase(p))) {
            s_next = (p + 1) % PHYS;
            return p;
        }
    }
    return -1;
}

/* Part k, its records already in s_buf after the header, into flash. */
static bool write_part(int k)
{
    const int p = blank_sector();
    if (p < 0) {
        return false;
    }
    const sector_hdr_t h = {
        .magic = SECTOR_MAGIC,
        .part = (uint16_t)k,
        .format = FORMAT,
        .record_size = sizeof(readings_slot_t),
        .seq = s_seq + 1,
        .crc = esp_rom_crc32_le(0, s_buf + HEADER_SIZE, SECTOR - HEADER_SIZE),
    };
    memcpy(s_buf, &h, sizeof(h));
    s_blank[p] = false;
    /* the header last: until it's there, the copy doesn't count */
    if (esp_partition_write(s_part, (size_t)p * SECTOR + HEADER_SIZE, s_buf + HEADER_SIZE,
                            SECTOR - HEADER_SIZE) != ESP_OK ||
        esp_partition_write(s_part, (size_t)p * SECTOR, s_buf, HEADER_SIZE) != ESP_OK) {
        return false;                   /* p is erased again before it's used */
    }
    s_seq = h.seq;
    const int old = s_where[k];
    s_where[k] = (int8_t)p;
    s_owner[p] = (int8_t)k;
    s_seq_of[p] = h.seq;
    if (old >= 0) {
        s_owner[old] = -1;
        erase(old);                     /* if that fails, the next boot drops it: its seq is lower */
    }
    return true;
}

void readings_store_flush(void)
{
    if (s_ram == NULL) {
        return;
    }
    xSemaphoreTake(s_flash, portMAX_DELAY);
    const int64_t t0 = esp_timer_get_time();
    bool ok = true;
    unsigned parts = 0;
    for (int k = 0; ok && k < PARTS; k++) {
        const uint32_t first = (uint32_t)k * PER_SECTOR;
        bool changed = false;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        if (s_state == READINGS_FLASH) {
            for (uint32_t i = 0; i < PER_SECTOR; i++) {
                if (marked(first + i)) {
                    changed = true;
                    unmark(first + i);
                }
            }
        }
        if (changed) {
            memcpy(s_buf + HEADER_SIZE, &s_ram[first], PER_SECTOR * sizeof(readings_slot_t));
        }
        xSemaphoreGive(s_mux);
        if (!changed) {
            continue;
        }
        ok = write_part(k);
        if (ok) {
            parts++;
        } else {
            xSemaphoreTake(s_mux, portMAX_DELAY);
            for (uint32_t i = 0; i < PER_SECTOR; i++) {
                if (s_ram[first + i].ts != 0) {
                    mark(first + i);    /* again next time */
                }
            }
            xSemaphoreGive(s_mux);
        }
    }
    xSemaphoreGive(s_flash);

    xSemaphoreTake(s_mux, portMAX_DELAY);
    const bool was_failing = s_failing;
    s_failing = !ok;
    xSemaphoreGive(s_mux);
    if (!ok && !was_failing) {
        ESP_LOGW(TAG, "can't write the \"%s\" partition; the readings wait in RAM", READINGS_PARTITION);
    } else if (parts != 0) {
        ESP_LOGI(TAG, "%u part%s into flash (%lld ms)", parts, parts == 1 ? "" : "s",
                 (long long)((esp_timer_get_time() - t0) / 1000));
    }
}

bool readings_store_latest(readings_slot_t *out)
{
    if (s_ram == NULL) {
        return false;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    const bool any = s_newest != 0;
    if (any) {
        *out = s_ram[s_newest % READINGS_SLOTS];
    }
    xSemaphoreGive(s_mux);
    return any;
}

void readings_store_cursor(char *out)
{
    uint32_t change = 0;
    if (s_ram != NULL) {
        xSemaphoreTake(s_mux, portMAX_DELAY);
        change = s_change;
        xSemaphoreGive(s_mux);
    }
    snprintf(out, READINGS_CURSOR_LEN, "%08lx-%lu", (unsigned long)s_boot, (unsigned long)change);
}

/* The change number a token stands for; 0 (all) for none, a malformed one,
 * or one from an earlier boot. */
static uint32_t change_of(const char *token)
{
    if (token == NULL || *token == '\0') {
        return 0;
    }
    char *end;
    const unsigned long boot = strtoul(token, &end, 16);
    if (*end != '-' || boot != s_boot) {
        return 0;
    }
    return (uint32_t)strtoul(end + 1, NULL, 10);
}

bool readings_store_changed(const char *after, uint32_t since, readings_fn fn, void *ctx)
{
    if (s_ram == NULL) {
        return true;
    }
    const uint32_t seen = change_of(after);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    const uint32_t newest = s_newest;
    xSemaphoreGive(s_mux);
    if (newest == 0) {
        return true;
    }
    /* the ring's span, oldest first; anything older at a position is past it */
    uint32_t slot = newest >= READINGS_SLOTS ? newest - READINGS_SLOTS + 1 : 1;
    while (slot <= newest) {
        readings_slot_t batch[BATCH];
        unsigned n = 0;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        for (; slot <= newest && n < BATCH; slot++) {
            const uint32_t pos = slot % READINGS_SLOTS;
            const readings_slot_t *s = &s_ram[pos];
            if (s->ts == slot * READINGS_SLOT_S && s->ts > since && s_changed[pos] > seen) {
                batch[n++] = *s;
            }
        }
        xSemaphoreGive(s_mux);
        for (unsigned i = 0; i < n; i++) {
            if (!fn(&batch[i], ctx)) {
                return false;
            }
        }
    }
    return true;
}

void readings_store_info(readings_store_info_t *out)
{
    memset(out, 0, sizeof(*out));       /* READINGS_OFF */
    if (s_ram == NULL) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    out->state = s_state;
    strlcpy(out->why, s_why, sizeof(out->why));
    out->write_failing = s_failing;
    out->newest = s_newest * READINGS_SLOT_S;
    for (uint32_t pos = 0; pos < READINGS_SLOTS; pos++) {
        const uint32_t ts = s_ram[pos].ts;
        if (ts == 0) {
            continue;
        }
        out->slots++;
        if (out->oldest == 0 || ts < out->oldest) {
            out->oldest = ts;
        }
        out->unwritten += marked(pos);
    }
    xSemaphoreGive(s_mux);
}

static bool all_erased(const uint8_t *buf)
{
    for (size_t i = 0; i < SECTOR; i++) {
        if (buf[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

/* A part's records from flash into RAM; the ones that don't fit their place
 * (never written, or from a different ring) read as empty. */
static void take_part(int k, const uint8_t *records)
{
    for (uint32_t i = 0; i < PER_SECTOR; i++) {
        const uint32_t pos = (uint32_t)k * PER_SECTOR + i;
        readings_slot_t s;
        memcpy(&s, records + i * sizeof(s), sizeof(s));
        const uint32_t slot = s.ts / READINGS_SLOT_S;
        if (s.ts == 0 || s.ts % READINGS_SLOT_S != 0 || slot % READINGS_SLOTS != pos || s.check != check_of(&s)) {
            memset(&s_ram[pos], 0, sizeof(s_ram[pos]));
            s_changed[pos] = 0;
            continue;
        }
        s_ram[pos] = s;
        s_changed[pos] = 1;
        if (slot > s_newest) {
            s_newest = slot;
        }
    }
}

/* The partition into RAM, at boot, before anyone else uses the store. */
static load_t load(void)
{
    memset(s_where, -1, sizeof(s_where));
    for (int p = 0; p < PHYS; p++) {
        s_owner[p] = -1;
        s_blank[p] = false;
        if (esp_partition_read(s_part, (size_t)p * SECTOR, s_buf, SECTOR) != ESP_OK) {
            return LOAD_FAILED;
        }
        sector_hdr_t h;
        memcpy(&h, s_buf, sizeof(h));
        if (h.magic == SECTOR_MAGIC && (h.format != FORMAT || h.record_size != sizeof(readings_slot_t))) {
            return LOAD_FOREIGN;
        }
        if (h.magic != SECTOR_MAGIC || h.part >= PARTS ||
            h.crc != esp_rom_crc32_le(0, s_buf + HEADER_SIZE, SECTOR - HEADER_SIZE)) {
            s_blank[p] = all_erased(s_buf);   /* otherwise erased before it's used */
            continue;
        }
        const int cur = s_where[h.part];
        if (cur >= 0 && s_seq_of[cur] > h.seq) {
            continue;                   /* an older copy: a power loss came before its erase */
        }
        if (cur >= 0) {
            s_owner[cur] = -1;          /* the older copy, read first */
        }
        s_where[h.part] = (int8_t)p;
        s_owner[p] = (int8_t)h.part;
        s_seq_of[p] = h.seq;
        if (h.seq > s_seq) {
            s_seq = h.seq;
            s_next = (p + 1) % PHYS;    /* carry on after the newest copy */
        }
        take_part(h.part, s_buf + HEADER_SIZE);
    }
    return LOAD_OK;
}

void readings_store_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    s_flash = xSemaphoreCreateMutex();
    s_ram = heap_caps_calloc(READINGS_SLOTS, sizeof(*s_ram), MALLOC_CAP_SPIRAM);
    s_changed = heap_caps_calloc(READINGS_SLOTS, sizeof(*s_changed), MALLOC_CAP_SPIRAM);
    s_buf = heap_caps_malloc(SECTOR, MALLOC_CAP_SPIRAM);
    if (s_mux == NULL || s_flash == NULL || s_ram == NULL || s_changed == NULL || s_buf == NULL) {
        ESP_LOGE(TAG, "no memory: the readings aren't kept");
        free(s_ram);
        free(s_changed);
        free(s_buf);
        s_ram = NULL;
        return;
    }
    s_boot = esp_random();

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, READINGS_PARTITION);
    if (s_part == NULL) {
        ram_only("no \"%s\" partition: the partition table with it isn't flashed", READINGS_PARTITION);
        return;
    }
    if (s_part->size < PHYS * SECTOR) {
        ram_only("the \"%s\" partition has %lu KB, %d KB needed", READINGS_PARTITION,
                 (unsigned long)(s_part->size / 1024), PHYS * SECTOR / 1024);
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    switch (load()) {
    case LOAD_OK:
        break;
    case LOAD_FOREIGN:
        ram_only("the \"%s\" partition holds a format this firmware doesn't know; left alone",
                 READINGS_PARTITION);
        return;
    case LOAD_FAILED:
        ram_only("can't read the \"%s\" partition", READINGS_PARTITION);
        return;
    }
    unsigned slots = 0, parts = 0;
    for (uint32_t pos = 0; pos < READINGS_SLOTS; pos++) {
        slots += s_ram[pos].ts != 0;
    }
    for (int k = 0; k < PARTS; k++) {
        parts += s_where[k] >= 0;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state = READINGS_FLASH;
    xSemaphoreGive(s_mux);
    ESP_LOGI(TAG, "%u slots in the \"%s\" partition (%u of %d parts written), read in %lld ms; newest %lu",
             slots, READINGS_PARTITION, parts, PARTS, (long long)((esp_timer_get_time() - t0) / 1000),
             (unsigned long)(s_newest * READINGS_SLOT_S));
}

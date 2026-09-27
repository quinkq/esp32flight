#include "runways.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "geo_math.h"
#include "esp_log.h"

static const char *TAG = "runways";

typedef struct {
    char  icao[5];
    float le_lat, le_lon, he_lat, he_lon;
} runway_t;

static runway_t *s_rw;
static int s_count = -1;    /* -1 = not loaded yet */

static void load(void)
{
    s_count = 0;
    FILE *f = fopen("/assets/runways.tsv", "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "no runway database");
        return;
    }
    int cap = 5000;
    s_rw = heap_caps_malloc(cap * sizeof(runway_t), MALLOC_CAP_SPIRAM);
    if (s_rw == NULL) {
        fclose(f);
        return;
    }
    char line[128];
    while (s_count < cap && fgets(line, sizeof(line), f) != NULL) {
        char icao[8];
        runway_t r;
        if (sscanf(line, "%7s\t%f\t%f\t%f\t%f",
                   icao, &r.le_lat, &r.le_lon, &r.he_lat, &r.he_lon) == 5) {
            strlcpy(r.icao, icao, sizeof(r.icao));
            s_rw[s_count++] = r;
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "loaded %d runways", s_count);
}

#define RW_COLOR 0x4E19   /* muted teal, readable on the dark map */

static inline void put_px(uint16_t *fb, int w, int h, int x, int y)
{
    if (x >= 0 && x < w && y >= 0 && y < h) {
        fb[(size_t)y * w + x] = RW_COLOR;
    }
}

/* 2 px thick Bresenham segment */
static void draw_line(uint16_t *fb, int w, int h, int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_px(fb, w, h, x0, y0);
        put_px(fb, w, h, x0 + 1, y0);
        put_px(fb, w, h, x0, y0 + 1);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void runways_draw(uint16_t *fb, int w, int h, const tile_view_t *view)
{
    if (s_count < 0) {
        load();
    }
    /* Cull with plain comparisons first. Projecting all ~4,200 runways
     * worldwide (two soft-float log/tan/cos each on the S3) held CPU 0 past
     * the 5 s task watchdog in the screensaver pre-render on the 7B. A
     * runway is skipped only when both ends lie beyond the same edge. */
    double la0, la1, lo0, lo1;
    bool lon_ok = tilemap_view_bounds(view, &la0, &la1, &lo0, &lo1);
    const float m = 0.1f;   /* margin, degrees */
    const float fla0 = (float)la0 - m, fla1 = (float)la1 + m;
    const float flo0 = (float)lo0 - m, flo1 = (float)lo1 + m;
    for (int i = 0; i < s_count; i++) {
        const runway_t *r = &s_rw[i];
        if ((r->le_lat < fla0 && r->he_lat < fla0) ||
            (r->le_lat > fla1 && r->he_lat > fla1)) {
            continue;
        }
        if (lon_ok && ((r->le_lon < flo0 && r->he_lon < flo0) ||
                       (r->le_lon > flo1 && r->he_lon > flo1))) {
            continue;
        }
        int x0, y0, x1, y1;
        tilemap_project(view, s_rw[i].le_lat, s_rw[i].le_lon, &x0, &y0);
        tilemap_project(view, s_rw[i].he_lat, s_rw[i].he_lon, &x1, &y1);
        /* skip strips fully off screen or degenerate at this zoom */
        if ((x0 < 0 && x1 < 0) || (x0 >= w && x1 >= w) ||
            (y0 < 0 && y1 < 0) || (y0 >= h && y1 >= h)) {
            continue;
        }
        if (abs(x1 - x0) < 2 && abs(y1 - y0) < 2) {
            continue;
        }
        draw_line(fb, w, h, x0, y0, x1, y1);
    }
}

static float ang_diff(float a, float b)
{
    float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f;
    return d < 0 ? -d : d;
}

bool runways_approach(double lat, double lon, float track_deg, int alt_ft,
                      int vs_fpm, float gs_kts, char *icao_out, size_t icao_len,
                      int *eta_min)
{
    if (alt_ft <= 0 || alt_ft > 10000 || vs_fpm > -200 || gs_kts < 60) {
        return false;
    }
    if (s_count < 0) {
        load();
    }
    double best_km = 35.0;
    int best = -1;
    for (int i = 0; i < s_count; i++) {
        double mlat = (s_rw[i].le_lat + s_rw[i].he_lat) / 2.0;
        double mlon = (s_rw[i].le_lon + s_rw[i].he_lon) / 2.0;
        double d = geo_haversine_km(lat, lon, mlat, mlon);
        if (d >= best_km) {
            continue;
        }
        float rwb = (float)geo_bearing_deg(s_rw[i].le_lat, s_rw[i].le_lon,
                                           s_rw[i].he_lat, s_rw[i].he_lon);
        float a = ang_diff(track_deg, rwb);
        if (a > 20.0f && a < 160.0f) {
            continue;       /* not aligned with the runway axis */
        }
        float to_rw = (float)geo_bearing_deg(lat, lon, mlat, mlon);
        if (ang_diff(track_deg, to_rw) > 30.0f) {
            continue;       /* aligned but flying away */
        }
        best_km = d;
        best = i;
    }
    if (best < 0) {
        return false;
    }
    strlcpy(icao_out, s_rw[best].icao, icao_len);
    *eta_min = (int)(best_km / (gs_kts * 1.852) * 60.0 + 0.5);
    return true;
}

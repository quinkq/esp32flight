/*
 * The weather station on the panel's web server (weather-station fork): the
 * outdoor unit's diagnostics and the stored readings. Every handler checks
 * the panel password, like the rest of the panel.
 *
 *   GET /outdoor                       a small page that refreshes itself
 *   GET /api/outdoor                   the outdoor unit as the display sees it, JSON
 *   GET /api/outdoor/events?since=T    the event log after unix time T, tab-separated
 *   GET /api/readings                  the newest stored slot (raw values) and the store's state, JSON
 *   GET /api/readings/history?cursor=C&since=T&fmt=json|csv|influx
 *                                      the slots changed since cursor C (none: all) with ts > T,
 *                                      oldest first; the next cursor in the X-Cursor header (and
 *                                      in the JSON). Raw values until the calibration settings.
 */
#include "weather_http.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "outdoor_link.h"
#include "readings_store.h"
#include "web_server.h"

static const char *TAG = "weather_http";

#define PAGE_EVENTS 10

static const char OUTDOOR_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Outdoor unit</title>"
"<style>"
"body{background:#0c1510;color:#e7f2ea;font-family:system-ui,sans-serif;margin:0 auto;padding:18px;max-width:1000px}"
"h1{color:#5dd39e;font-size:22px;margin:0 0 4px}"
"h3{font-size:16px;margin:20px 0 4px}"
"a{color:#5dd39e}"
".dim{color:#86a695;font-size:14px}"
".st{font-size:17px;margin:12px 0 4px}"
".cards{display:flex;gap:10px;flex-wrap:wrap;margin:10px 0}"
".card{background:#14231b;border:1px solid #23402f;border-radius:12px;padding:10px 14px;font-size:13px;color:#86a695}"
".card b{display:block;color:#e7f2ea;font-size:17px}"
".ok{color:#5dd39e}.late{color:#ffd166}.silent,.bad{color:#ff6b6b}"
"p{margin:6px 0;font-size:14px}"
"table{border-collapse:collapse;font-size:14px;width:100%}"
"td{padding:5px 8px;border-bottom:1px solid #14231b;vertical-align:top}"
"td.t{color:#86a695;white-space:nowrap}"
"</style></head><body>"
"<h1>Outdoor unit</h1>"
"<div class='dim'><a href='/'>&larr; panel</a> &middot; <a href='/api/outdoor/events'>full event log</a></div>"
"<div id='st' class='st'>&hellip;</div>"
"<div id='cards' class='cards'></div>"
"<p id='frame'></p><p id='counts'></p>"
"<h3>Recent events</h3><table id='ev'></table>"
"<script>"
"const R=['unknown','power-on','reset pin','software restart','panic','interrupt watchdog',"
"'task watchdog','other watchdog','deep-sleep wake','brownout','SDIO','USB','JTAG','eFuse',"
"'power glitch','CPU lockup'];"
"const $=id=>document.getElementById(id);"
"function dur(s){s=Math.max(0,Math.round(s));if(s<90)return s+' s';if(s<5400)return Math.round(s/60)+' min';"
"const h=Math.floor(s/3600);return h+' h '+Math.round((s-h*3600)/60)+' min';}"
"function hm(t){return new Date(t*1000).toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'});}"
"function esc(s){return String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}"
"function card(k,v){return '<div class=card>'+k+'<b>'+v+'</b></div>';}"
"function ok(name,good){return '<span class='+(good?'ok':'bad')+'>'+name+' '+(good?'&#10003;':'&#10007;')+'</span>';}"
"async function tick(){"
"let d;try{d=await (await fetch('/api/outdoor')).json();}catch(e){$('st').textContent='display not reachable';return;}"
"if(!d.running){$('st').innerHTML='<span class=bad>&#9679; link off</span> (not paired in this build)';return;}"
"let s='<span class='+d.status.replace(' ','')+'>&#9679; '+d.status+'</span>';"
"if(d.last_heard_s_ago!==undefined){s+=' &middot; last heard '+dur(d.last_heard_s_ago)+' ago';"
"s+=d.next_due_s>=0?' &middot; next frame in '+dur(d.next_due_s):' &middot; overdue by '+dur(-d.next_due_s);}"
"$('st').innerHTML=s;"
"const c=d.counts,f=d.frame;"
"if(f){"
"$('cards').innerHTML="
"card('Battery',f.bat_mv==null?'unknown':(f.bat_mv/1000).toFixed(2)+' V'+(f.low_battery?' (low)':''))+"
"card('Signal',f.rssi+' dBm')+"
"card('Sensors',ok('SHT40',f.sht_ok)+' &nbsp;'+ok('BME280',f.bme_ok))+"
"card('Firmware','build '+f.fw_build)+"
"card('Clock',(f.clock_synced?'synced':'not set')+(f.clock_offset_s==null?'':', '+(f.clock_offset_s>=0?'+':'')+f.clock_offset_s+' s'))+"
"card('Channel',c.channel||'&ndash;')+"
"card('Free space',d.assets_free_kb==null?'&ndash;':'<span class='+(d.assets_free_kb<100?'bad':'')+'>'+d.assets_free_kb+' KB</span>');"
"$('frame').innerHTML='Last frame: '+f.records+' reading'+(f.records==1?'':'s')"
"+(f.reset_reason!=8?' &middot; it started by '+(R[f.reset_reason]||'reset '+f.reset_reason):'')"
"+(f.backlog_dropped?' &middot; <span class=bad>its backlog overflowed</span>':'')"
"+' &middot; since its previous frame: awake '+f.awake_total_ms+' ms, '+f.failed_searches+' failed searches, '"
"+f.resets+' resets, '+f.brownouts+' brownouts';}"
"$('counts').innerHTML='Since the display started ('+dur(d.uptime_s)+'): '+c.frames+' frames, '+c.rejected+' rejected, '"
"+c.unanswered+' unanswered, '+c.replies_unacked+' replies unconfirmed &middot; '+c.readings+' readings, '"
"+c.late+' late, '+c.missing+' missing, '+c.twice+' twice &middot; '+c.channel_changes+' channel changes';"
"$('ev').innerHTML=d.events.map(e=>'<tr><td class=t>'+(e.time?hm(e.time):'&ndash;')+'</td><td class=t>'"
"+esc(e.kind)+'</td><td>'+esc(e.text)+'</td></tr>').join('');}"
"tick();setInterval(tick,10000);"
"</script></body></html>";

static esp_err_t outdoor_page_get(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return require_auth(req);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, OUTDOOR_HTML, sizeof(OUTDOOR_HTML) - 1);
}

static const char *status_name(outdoor_status_t s)
{
    switch (s) {
    case OUTDOOR_OK:
        return "ok";
    case OUTDOOR_LATE:
        return "late";
    case OUTDOOR_SILENT:
        return "silent";
    default:
        return "not heard";
    }
}

static esp_err_t outdoor_api_get(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return require_auth(req);
    }
    outdoor_health_t h;
    outdoor_link_health(&h);
    const int64_t now_us = esp_timer_get_time();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "running", h.running);
    cJSON_AddStringToObject(root, "status", status_name(h.status));
    cJSON_AddNumberToObject(root, "expected_gap_s", h.expected_gap_s);
    cJSON_AddNumberToObject(root, "uptime_s", h.uptime_s);
    if (h.last_heard_us != 0) {
        const int64_t ago_s = (now_us - h.last_heard_us) / 1000000;
        cJSON_AddNumberToObject(root, "last_heard_s_ago", (double)ago_s);
        cJSON_AddNumberToObject(root, "next_due_s", (double)((int64_t)h.expected_gap_s - ago_s));

        cJSON *f = cJSON_AddObjectToObject(root, "frame");
        cJSON_AddNumberToObject(f, "fw_build", h.fw_build);
        cJSON_AddNumberToObject(f, "reset_reason", h.reset_reason);
        cJSON_AddBoolToObject(f, "clock_synced", h.clock_synced);
        cJSON_AddBoolToObject(f, "low_battery", h.low_battery);
        cJSON_AddBoolToObject(f, "backlog_dropped", h.backlog_dropped);
        cJSON_AddBoolToObject(f, "sht_ok", h.sht_ok);
        cJSON_AddBoolToObject(f, "bme_ok", h.bme_ok);
        if (h.bat_mv != 0xFFFF) {
            cJSON_AddNumberToObject(f, "bat_mv", h.bat_mv);
        } else {
            cJSON_AddNullToObject(f, "bat_mv");
        }
        cJSON_AddNumberToObject(f, "rssi", h.rssi);
        cJSON_AddNumberToObject(f, "records", h.records);
        if (h.offset_known) {
            cJSON_AddNumberToObject(f, "clock_offset_s", (double)h.clock_offset_s);
        } else {
            cJSON_AddNullToObject(f, "clock_offset_s");
        }
        cJSON_AddNumberToObject(f, "awake_total_ms", h.awake_total_ms);
        cJSON_AddNumberToObject(f, "brownouts", h.brownouts);
        cJSON_AddNumberToObject(f, "resets", h.resets);
        cJSON_AddNumberToObject(f, "failed_searches", h.failed_searches);
    }

    cJSON *c = cJSON_AddObjectToObject(root, "counts");
    cJSON_AddNumberToObject(c, "frames", h.frames);
    cJSON_AddNumberToObject(c, "rejected", h.rejected);
    cJSON_AddNumberToObject(c, "unanswered", h.unanswered);
    cJSON_AddNumberToObject(c, "replies_unacked", h.replies_unacked);
    cJSON_AddNumberToObject(c, "readings", h.readings);
    cJSON_AddNumberToObject(c, "late", h.late);
    cJSON_AddNumberToObject(c, "missing", h.missing);
    cJSON_AddNumberToObject(c, "twice", h.twice);
    cJSON_AddNumberToObject(c, "channel", h.channel);
    cJSON_AddNumberToObject(c, "channel_changes", h.channel_changes);
    if (h.assets_free_kb >= 0) {
        cJSON_AddNumberToObject(root, "assets_free_kb", h.assets_free_kb);
    } else {
        cJSON_AddNullToObject(root, "assets_free_kb");
    }

    /* from RAM, so they show before the display has time to write them */
    cJSON *events = cJSON_AddArrayToObject(root, "events");
    outdoor_event_t *ev = heap_caps_malloc(PAGE_EVENTS * sizeof(*ev), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ev != NULL) {
        const size_t n = outdoor_link_events(ev, PAGE_EVENTS);
        for (size_t i = 0; i < n; i++) {
            cJSON *e = cJSON_CreateObject();
            if (ev[i].unix_time != 0) {
                cJSON_AddNumberToObject(e, "time", (double)ev[i].unix_time);
            } else {
                cJSON_AddNullToObject(e, "time");
            }
            cJSON_AddStringToObject(e, "kind", ev[i].kind);
            cJSON_AddStringToObject(e, "text", ev[i].text);
            cJSON_AddItemToArray(events, e);
        }
        free(ev);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_send(req, json ? json : "{}", HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

static esp_err_t outdoor_events_get(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return require_auth(req);
    }
    long long since = 0;
    char query[48], val[24];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
        since = strtoll(val, NULL, 10);
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    const char *paths[] = { OUTDOOR_LOG_OLD_PATH, OUTDOOR_LOG_PATH };
    char line[320];                     /* longer than any line the log writes */
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *f = fopen(paths[i], "r");
        if (f == NULL) {
            continue;
        }
        while (fgets(line, sizeof(line), f) != NULL) {
            if (strtoll(line, NULL, 10) > since &&
                httpd_resp_send_chunk(req, line, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
                fclose(f);
                return ESP_FAIL;
            }
        }
        fclose(f);
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static const char *store_state_name(readings_store_state_t s)
{
    switch (s) {
    case READINGS_FLASH:
        return "flash";
    case READINGS_RAM_ONLY:
        return "ram only";
    default:
        return "off";
    }
}

/* a value in its unit, or null when it's missing */
static void add_value(cJSON *o, const char *key, bool ok, double v)
{
    if (ok) {
        cJSON_AddNumberToObject(o, key, v);
    } else {
        cJSON_AddNullToObject(o, key);
    }
}

static esp_err_t readings_api_get(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return require_auth(req);
    }
    readings_store_info_t si;
    readings_store_info(&si);

    cJSON *root = cJSON_CreateObject();
    cJSON *st = cJSON_AddObjectToObject(root, "store");
    cJSON_AddStringToObject(st, "state", store_state_name(si.state));
    if (si.state == READINGS_RAM_ONLY) {
        cJSON_AddStringToObject(st, "why", si.why);
    }
    cJSON_AddBoolToObject(st, "write_failing", si.write_failing);
    cJSON_AddNumberToObject(st, "slots", si.slots);
    cJSON_AddNumberToObject(st, "unwritten", si.unwritten);
    add_value(st, "oldest", si.oldest != 0, si.oldest);
    add_value(st, "newest", si.newest != 0, si.newest);

    readings_slot_t s;
    if (readings_store_latest(&s)) {
        /* raw: calibration comes with the settings */
        cJSON *o = cJSON_AddObjectToObject(root, "outdoor");
        const bool sht = s.flags & READINGS_SHT_OK, bme = s.flags & READINGS_BME_OK;
        cJSON_AddNumberToObject(o, "ts", s.ts);
        add_value(o, "sht_t", sht, s.sht_t / 100.0);
        add_value(o, "sht_rh", sht, s.sht_rh / 100.0);
        add_value(o, "bme_t", bme, s.bme_t / 100.0);
        add_value(o, "bme_rh", bme, s.bme_rh / 100.0);
        add_value(o, "bme_p", bme, s.bme_p / 10.0);
        add_value(o, "bat_mv", s.bat_mv != WS_LINK_NO_U16, s.bat_mv);
        add_value(o, "rssi", s.rssi != READINGS_NO_RSSI, s.rssi);
        cJSON_AddNumberToObject(o, "readings", s.count);
        cJSON_AddBoolToObject(o, "late", s.flags & READINGS_LATE);
        cJSON_AddBoolToObject(o, "low_battery", s.flags & READINGS_LOW_BATTERY);
    } else {
        cJSON_AddNullToObject(root, "outdoor");
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_send(req, json ? json : "{}", HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

typedef enum { FMT_JSON, FMT_CSV, FMT_INFLUX } fmt_t;

#define OUT_SIZE 2048                   /* sent in chunks of up to this */
#define OUT_PIECE 320                   /* no single piece is longer (a JSON slot: ~230) */

typedef struct {
    httpd_req_t *req;
    fmt_t fmt;
    bool  first;                        /* JSON: no comma before the first slot */
    bool  failed;                       /* the client went away */
    size_t len;
    char  buf[OUT_SIZE];
} out_t;

static void out_flush(out_t *o)
{
    if (!o->failed && o->len > 0) {
        o->failed = httpd_resp_send_chunk(o->req, o->buf, o->len) != ESP_OK;
    }
    o->len = 0;
}

static void out_printf(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void out_printf(out_t *o, const char *fmt, ...)
{
    if (OUT_SIZE - o->len < OUT_PIECE) {
        out_flush(o);
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(o->buf + o->len, OUT_SIZE - o->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        o->len += (size_t)n < OUT_SIZE - o->len ? (size_t)n : OUT_SIZE - o->len - 1;
    }
}

/* v with the given decimals, or the text for a missing value */
static void val(char *buf, size_t n, bool ok, double v, int decimals, const char *missing)
{
    if (ok) {
        snprintf(buf, n, "%.*f", decimals, v);
    } else {
        strlcpy(buf, missing, n);
    }
}

/* key=value onto an Influx line's field list */
static void add_field(char *list, size_t n, const char *key, const char *value)
{
    const size_t len = strlen(list);
    snprintf(list + len, n - len, "%s%s=%s", len != 0 ? "," : "", key, value);
}

static void put_influx(out_t *o, const readings_slot_t *s)
{
    char v[16], f[OUT_PIECE - 32] = "";
    if (s->flags & READINGS_SHT_OK) {
        val(v, sizeof(v), true, s->sht_t / 100.0, 2, "");
        add_field(f, sizeof(f), "temp_c", v);
        val(v, sizeof(v), true, s->sht_rh / 100.0, 2, "");
        add_field(f, sizeof(f), "rh", v);
    }
    if (s->flags & READINGS_BME_OK) {
        val(v, sizeof(v), true, s->bme_t / 100.0, 2, "");
        add_field(f, sizeof(f), "temp_bme_c", v);
        val(v, sizeof(v), true, s->bme_rh / 100.0, 2, "");
        add_field(f, sizeof(f), "rh_bme", v);
        val(v, sizeof(v), true, s->bme_p / 10.0, 1, "");
        add_field(f, sizeof(f), "pressure_hpa", v);
    }
    if (f[0] != '\0') {
        out_printf(o, "ws_outdoor %s %lu\n", f, (unsigned long)s->ts);
    }
    if (s->flags & READINGS_INDOOR_OK) {
        out_printf(o, "ws_indoor temp_c=%.2f,rh=%.2f %lu\n", s->in_t / 100.0, s->in_rh / 100.0,
                   (unsigned long)s->ts);
    }
    f[0] = '\0';
    if (s->bat_mv != WS_LINK_NO_U16) {
        val(v, sizeof(v), true, s->bat_mv / 1000.0, 3, "");
        add_field(f, sizeof(f), "bat_v", v);
    }
    if (s->rssi != READINGS_NO_RSSI) {
        snprintf(v, sizeof(v), "%di", s->rssi);
        add_field(f, sizeof(f), "rssi", v);
    }
    snprintf(v, sizeof(v), "%ui", s->count);
    add_field(f, sizeof(f), "readings", v);
    add_field(f, sizeof(f), "late", s->flags & READINGS_LATE ? "true" : "false");
    add_field(f, sizeof(f), "low_battery", s->flags & READINGS_LOW_BATTERY ? "true" : "false");
    out_printf(o, "ws_outdoor_unit %s %lu\n", f, (unsigned long)s->ts);
}

/* CSV and JSON share the columns; only the missing-value text and the framing differ */
static void put_row(out_t *o, const readings_slot_t *s)
{
    const bool json = o->fmt == FMT_JSON;
    const char *none = json ? "null" : "";
    const bool sht = s->flags & READINGS_SHT_OK, bme = s->flags & READINGS_BME_OK;
    const bool in = s->flags & READINGS_INDOOR_OK;
    char sht_t[12], sht_rh[12], bme_t[12], bme_rh[12], bme_p[12], bat[8], rssi[8], in_t[12], in_rh[12];
    val(sht_t, sizeof(sht_t), sht, s->sht_t / 100.0, 2, none);
    val(sht_rh, sizeof(sht_rh), sht, s->sht_rh / 100.0, 2, none);
    val(bme_t, sizeof(bme_t), bme, s->bme_t / 100.0, 2, none);
    val(bme_rh, sizeof(bme_rh), bme, s->bme_rh / 100.0, 2, none);
    val(bme_p, sizeof(bme_p), bme, s->bme_p / 10.0, 1, none);
    val(bat, sizeof(bat), s->bat_mv != WS_LINK_NO_U16, s->bat_mv, 0, none);
    val(rssi, sizeof(rssi), s->rssi != READINGS_NO_RSSI, s->rssi, 0, none);
    val(in_t, sizeof(in_t), in, s->in_t / 100.0, 2, none);
    val(in_rh, sizeof(in_rh), in, s->in_rh / 100.0, 2, none);
    const bool late = s->flags & READINGS_LATE, low = s->flags & READINGS_LOW_BATTERY;
    if (json) {
        out_printf(o, "%s{\"ts\":%lu,\"sht_t\":%s,\"sht_rh\":%s,\"bme_t\":%s,\"bme_rh\":%s,\"bme_p\":%s,"
                      "\"bat_mv\":%s,\"rssi\":%s,\"readings\":%u,\"late\":%s,\"low_battery\":%s,"
                      "\"in_t\":%s,\"in_rh\":%s}",
                   o->first ? "" : ",", (unsigned long)s->ts, sht_t, sht_rh, bme_t, bme_rh, bme_p, bat, rssi,
                   s->count, late ? "true" : "false", low ? "true" : "false", in_t, in_rh);
        o->first = false;
    } else {
        char when[24];
        const time_t t = s->ts;
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
        out_printf(o, "%lu,%s,%s,%s,%s,%s,%s,%s,%s,%u,%d,%d,%s,%s\n", (unsigned long)s->ts, when, sht_t, sht_rh,
                   bme_t, bme_rh, bme_p, bat, rssi, s->count, late, low, in_t, in_rh);
    }
}

static bool put_slot(const readings_slot_t *s, void *ctx)
{
    out_t *o = ctx;
    if (o->fmt == FMT_INFLUX) {
        put_influx(o, s);
    } else {
        put_row(o, s);
    }
    return !o->failed;
}

static esp_err_t history_get(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return require_auth(req);
    }
    char query[128], cursor[READINGS_CURSOR_LEN] = "", v[16];
    uint32_t since = 0;
    fmt_t fmt = FMT_JSON;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "cursor", cursor, sizeof(cursor)) != ESP_OK) {
            cursor[0] = '\0';
        }
        if (httpd_query_key_value(query, "since", v, sizeof(v)) == ESP_OK) {
            since = strtoul(v, NULL, 10);
        }
        if (httpd_query_key_value(query, "fmt", v, sizeof(v)) == ESP_OK) {
            if (strcmp(v, "csv") == 0) {
                fmt = FMT_CSV;
            } else if (strcmp(v, "influx") == 0) {
                fmt = FMT_INFLUX;
            } else if (strcmp(v, "json") != 0) {
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "fmt: json, csv or influx");
            }
        }
    }
    out_t *o = heap_caps_calloc(1, sizeof(*o), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (o == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    o->req = req;
    o->fmt = fmt;
    o->first = true;

    /* before the slots: one that changes meanwhile comes again next time */
    char next[READINGS_CURSOR_LEN];
    readings_store_cursor(next);
    httpd_resp_set_hdr(req, "X-Cursor", next);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, fmt == FMT_JSON ? "application/json"
                           : fmt == FMT_CSV ? "text/csv; charset=utf-8" : "text/plain; charset=utf-8");
    if (fmt == FMT_JSON) {
        out_printf(o, "{\"cursor\":\"%s\",\"slots\":[", next);
    } else if (fmt == FMT_CSV) {
        out_printf(o, "ts,time,sht_t,sht_rh,bme_t,bme_rh,bme_p,bat_mv,rssi,readings,late,low_battery,in_t,in_rh\n");
    }
    readings_store_changed(cursor, since, put_slot, o);
    if (fmt == FMT_JSON) {
        out_printf(o, "]}");
    }
    out_flush(o);
    const bool failed = o->failed;
    free(o);
    return failed ? ESP_FAIL : httpd_resp_send_chunk(req, NULL, 0);
}

void weather_http_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/outdoor", .method = HTTP_GET, .handler = outdoor_page_get },
        { .uri = "/api/outdoor", .method = HTTP_GET, .handler = outdoor_api_get },
        { .uri = "/api/outdoor/events", .method = HTTP_GET, .handler = outdoor_events_get },
        { .uri = "/api/readings", .method = HTTP_GET, .handler = readings_api_get },
        { .uri = "/api/readings/history", .method = HTTP_GET, .handler = history_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(server, &uris[i]) != ESP_OK) {
            ESP_LOGW(TAG, "%s not registered (max_uri_handlers too low?)", uris[i].uri);
        }
    }
}

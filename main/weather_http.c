/*
 * The weather station on the panel's web server (weather-station fork): the
 * outdoor unit's diagnostics, the stored readings and the weather station
 * settings. Every handler checks the panel password, like the rest of the
 * panel.
 *
 *   GET /outdoor                       the weather station's page: diagnostics, refreshing
 *                                      itself, and the settings form
 *   GET /api/outdoor                   the outdoor unit as the display sees it, JSON
 *   GET /api/outdoor/events?since=T    the event log after unix time T, tab-separated
 *   GET /api/readings?raw=1            the newest stored outdoor slot, the indoor sensor's latest
 *                                      sample and the store's state, JSON
 *   GET /api/readings/history?cursor=C&since=T&fmt=json|csv|influx&raw=1
 *                                      the slots changed since cursor C (none: all) with ts > T,
 *                                      oldest first; the next cursor in the X-Cursor header (and
 *                                      in the JSON)
 *   GET/POST /api/weather-station/settings
 *                                      the weather station settings, JSON; the keys are never
 *                                      sent back. A new pairing or link switch restarts the display.
 *
 * Readings come calibrated (offsets, battery factor, pressure at sea level
 * when the altitude is known); raw=1 gives the stored values.
 */
#include "weather_http.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "altitude.h"
#include "calibration.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "indoor.h"
#include "outdoor_link.h"
#include "readings_store.h"
#include "sdkconfig.h"
#include "settings.h"
#include "web_server.h"
#include "ws_settings.h"

static const char *TAG = "weather_http";

#define PAGE_EVENTS 10

static const char OUTDOOR_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Weather station</title>"
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
"fieldset{border:1px solid #23402f;border-radius:12px;margin:10px 0;padding:8px 14px}"
"legend{color:#86a695;font-size:14px}"
"label{display:block;margin:7px 0;font-size:14px}"
"input{background:#14231b;color:#e7f2ea;border:1px solid #23402f;border-radius:6px;padding:4px 6px;font-size:14px}"
"input[type=number]{width:90px}input.w{width:230px}"
"button{background:#5dd39e;color:#0c1510;border:0;border-radius:8px;padding:8px 18px;font-size:15px;cursor:pointer}"
"</style></head><body>"
"<h1>Weather station</h1>"
"<div class='dim'><a href='/'>&larr; panel</a> &middot; <a href='/api/outdoor/events'>full event log</a>"
" &middot; <a href='#set'>settings</a></div>"
"<h3>Outdoor unit</h3>"
"<div id='st' class='st'>&hellip;</div>"
"<div id='cards' class='cards'></div>"
"<p id='frame'></p><p id='counts'></p>"
"<h3>Recent events</h3><table id='ev'></table>"
"<h3 id='set'>Settings</h3>"
"<form id='sf' onsubmit='return save(event)'>"
"<fieldset><legend>Outdoor unit link</legend>"
"<label><input type=checkbox id='s_link'> link on</label>"
"<label>Outdoor unit's MAC <input id='s_mac' class=w placeholder='aa:bb:cc:dd:ee:ff'></label>"
"<label>PMK <input id='s_pmk' class=w type=password autocomplete=off></label>"
"<label>LMK <input id='s_lmk' class=w type=password autocomplete=off></label>"
"<div class=dim id='s_keys'></div></fieldset>"
"<fieldset><legend>Measurements</legend>"
"<label>The outdoor unit measures every <input id='s_int' type=number step=1 max=360> minutes (3 to 360)</label>"
"<label><input type=checkbox id='s_in'> indoor sensor on</label></fieldset>"
"<fieldset><legend>Calibration</legend>"
"<label>Outdoor temperature <input id='s_ot' type=number step=0.1 min=-5 max=5> &deg;C</label>"
"<label>Outdoor humidity <input id='s_orh' type=number step=0.1 min=-10 max=10> %</label>"
"<label>Indoor temperature <input id='s_it' type=number step=0.1 min=-5 max=5> &deg;C</label>"
"<label>Indoor humidity <input id='s_irh' type=number step=0.1 min=-10 max=10> %</label>"
"<label>Outdoor unit's battery voltage <input id='s_bat' type=number step=1 min=-50 max=50> &permil;</label>"
"<div class=dim>Added to what the sensors measure. The stored values stay raw, so a change corrects the whole history.</div>"
"</fieldset>"
"<fieldset><legend>Altitude, for pressure at sea level</legend>"
"<div id='s_alt' class=dim></div>"
"<label>Override <input id='s_alto' type=number step=1 min=-100 max=3000> m (empty: automatic)</label>"
"</fieldset>"
"<button>Save</button> <span id='s_msg'></span></form>"
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
"if(!d.running){$('st').innerHTML='<span class=bad>&#9679; link off</span> (switched off, not paired, or not in this build: see the settings below)';return;}"
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
"const U='/api/weather-station/settings';"
"function num(id){const v=$(id).value;return v===''?null:Number(v);}"
"async function load(){let d;try{d=await (await fetch(U)).json();}catch(e){$('s_msg').textContent='settings not reachable';return;}"
"$('s_link').checked=d.link_on;$('s_mac').value=d.mac;$('s_pmk').value='';$('s_lmk').value='';"
"$('s_pmk').placeholder=$('s_lmk').placeholder=d.paired?'set (empty: keep)':'32 hex digits';"
"$('s_keys').textContent=(d.link_in_build?'':'This build has no link to the outdoor unit. ')"
"+'Changing the link switch, the MAC or a key restarts the display.';"
"$('s_int').value=Math.round(d.interval_s/60);$('s_int').min=d.interval_s>=180?3:1;"
"$('s_in').checked=d.indoor_on;"
"$('s_ot').value=d.cal_out_t;$('s_orh').value=d.cal_out_rh;$('s_it').value=d.cal_in_t;$('s_irh').value=d.cal_in_rh;"
"$('s_bat').value=d.bat_trim_pm;$('s_alto').value=d.alt_override_m==null?'':d.alt_override_m;"
"const at=d.alt_fixed_home?d.alt_home_lat.toFixed(5)+', '+d.alt_home_lon.toFixed(5):'';"
"$('s_alt').innerHTML=(d.alt_fixed_home?(d.alt_auto_m==null?'Looking up the altitude of the home location '+at+'&hellip; ':"
"'The home location '+at+' is at '+d.alt_auto_m+' m (Open-Meteo). '):"
"'The home location comes from the internet address, too rough for the altitude: set fixed coordinates on the <a href=\"/\">panel</a>&rsquo;s Location card. ')"
"+(d.alt_used_m==null?'Pressure is shown as measured.':'Pressure is reduced to sea level from '+d.alt_used_m+' m.');}"
"async function save(ev){ev.preventDefault();const b={link_on:$('s_link').checked,mac:$('s_mac').value.trim(),"
"interval_s:Math.round(num('s_int')*60),indoor_on:$('s_in').checked,cal_out_t:num('s_ot')||0,cal_out_rh:num('s_orh')||0,"
"cal_in_t:num('s_it')||0,cal_in_rh:num('s_irh')||0,bat_trim_pm:num('s_bat')||0,alt_override_m:num('s_alto')};"
"if($('s_pmk').value)b.pmk=$('s_pmk').value.trim();if($('s_lmk').value)b.lmk=$('s_lmk').value.trim();"
"$('s_msg').textContent='saving...';"
"let r;try{r=await fetch(U,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});}"
"catch(e){$('s_msg').textContent='display not reachable';return false;}"
"if(!r.ok){$('s_msg').innerHTML='<span class=bad>'+esc(await r.text())+'</span>';return false;}"
"const a=await r.json();"
"if(a.restart){$('s_msg').textContent='Saved. The display restarts; this page reloads in 20 s.';setTimeout(()=>location.reload(),20000);}"
"else{$('s_msg').textContent='Saved.';load();}return false;}"
"tick();setInterval(tick,10000);load();"
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

/* ?raw=1 in a query: the stored values, without the calibration */
static bool raw_in(const char *query)
{
    char v[4];
    return httpd_query_key_value(query, "raw", v, sizeof(v)) == ESP_OK && strcmp(v, "1") == 0;
}

static esp_err_t readings_api_get(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return require_auth(req);
    }
    char query[32] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    const bool raw = raw_in(query);
    calibration_t cal;
    calibration_get(&cal);
    readings_store_info_t si;
    readings_store_info(&si);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "calibrated", !raw);
    cJSON_AddBoolToObject(root, "pressure_at_sea_level", !raw && cal.have_alt);
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
        if (!raw) {
            calibration_apply(&cal, &s);
        }
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

    /* the latest sample; the store has the slot means */
    indoor_t in;
    indoor_latest(&in);
    if (in.have && !raw) {
        calibration_indoor(&cal, &in.t, &in.rh);
    }
    cJSON *i = cJSON_AddObjectToObject(root, "indoor");
    cJSON_AddBoolToObject(i, "on", in.on);
    cJSON_AddBoolToObject(i, "answering", in.answering);
    if (in.serial != 0) {
        char serial[12];
        snprintf(serial, sizeof(serial), "%08lx", (unsigned long)in.serial);
        cJSON_AddStringToObject(i, "serial", serial);
    } else {
        cJSON_AddNullToObject(i, "serial");
    }
    add_value(i, "age_s", in.have, in.age_s);
    add_value(i, "t", in.have, in.t / 100.0);
    add_value(i, "rh", in.have, in.rh / 100.0);

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
    bool  raw;                          /* the stored values, without cal */
    calibration_t cal;
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

static bool put_slot(const readings_slot_t *stored, void *ctx)
{
    out_t *o = ctx;
    readings_slot_t s = *stored;
    if (!o->raw) {
        calibration_apply(&o->cal, &s);
    }
    if (o->fmt == FMT_INFLUX) {
        put_influx(o, &s);
    } else {
        put_row(o, &s);
    }
    return !o->failed;
}

static esp_err_t history_get(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return require_auth(req);
    }
    char query[128] = "", cursor[READINGS_CURSOR_LEN] = "", v[16];
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
    o->raw = raw_in(query);
    calibration_get(&o->cal);
    o->first = true;

    /* before the slots: one that changes meanwhile comes again next time */
    char next[READINGS_CURSOR_LEN];
    readings_store_cursor(next);
    httpd_resp_set_hdr(req, "X-Cursor", next);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, fmt == FMT_JSON ? "application/json"
                           : fmt == FMT_CSV ? "text/csv; charset=utf-8" : "text/plain; charset=utf-8");
    if (fmt == FMT_JSON) {
        out_printf(o, "{\"cursor\":\"%s\",\"calibrated\":%s,\"pressure_at_sea_level\":%s,\"slots\":[", next,
                   o->raw ? "false" : "true", !o->raw && o->cal.have_alt ? "true" : "false");
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

static esp_err_t ws_settings_api_get(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return require_auth(req);
    }
    ws_settings_t cfg;
    ws_settings_get(&cfg);
#if CONFIG_OUTDOOR_LINK_ENABLED
    const bool in_build = true;
#else
    const bool in_build = false;
#endif
    char mac[18] = "";
    if (memcmp(cfg.mac, (const uint8_t[6]){0}, sizeof(cfg.mac)) != 0) {
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", cfg.mac[0], cfg.mac[1], cfg.mac[2],
                 cfg.mac[3], cfg.mac[4], cfg.mac[5]);
    }
    ws_settings_t looked_up = cfg;
    looked_up.alt_override_m = WS_ALT_AUTO;
    int auto_m = 0, used_m = 0;
    const bool have_auto = altitude_m(&looked_up, &auto_m);
    const bool have_used = altitude_m(&cfg, &used_m);
    const settings_t *home = settings_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "link_in_build", in_build);
    cJSON_AddBoolToObject(root, "link_on", cfg.link_on);
    cJSON_AddBoolToObject(root, "paired", ws_settings_paired(&cfg));   /* the keys themselves never */
    cJSON_AddStringToObject(root, "mac", mac);
    cJSON_AddNumberToObject(root, "interval_s", cfg.interval_s);
    cJSON_AddBoolToObject(root, "indoor_on", cfg.indoor_on);
    cJSON_AddNumberToObject(root, "cal_out_t", cfg.cal_out_t / 100.0);
    cJSON_AddNumberToObject(root, "cal_out_rh", cfg.cal_out_rh / 100.0);
    cJSON_AddNumberToObject(root, "cal_in_t", cfg.cal_in_t / 100.0);
    cJSON_AddNumberToObject(root, "cal_in_rh", cfg.cal_in_rh / 100.0);
    cJSON_AddNumberToObject(root, "bat_trim_pm", cfg.bat_trim_pm);
    add_value(root, "alt_override_m", cfg.alt_override_m != WS_ALT_AUTO, cfg.alt_override_m);
    cJSON_AddBoolToObject(root, "alt_fixed_home", home->use_fixed_loc);
    cJSON_AddNumberToObject(root, "alt_home_lat", home->lat);
    cJSON_AddNumberToObject(root, "alt_home_lon", home->lon);
    add_value(root, "alt_auto_m", have_auto, auto_m);
    add_value(root, "alt_used_m", have_used, used_m);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, json ? json : "{}", HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

/* A number in a unit into hundredths (or units, scale 1); false if it's
 * absent. *why when it can't be stored at all. */
static bool scaled(const cJSON *root, const char *key, double scale, int16_t *out, const char **why)
{
    const cJSON *j = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(j)) {
        return false;
    }
    const long v = lround(j->valuedouble * scale);
    if (v < -30000 || v > 30000) {
        *why = "a value is far out of range";
        return false;
    }
    *out = (int16_t)v;
    return true;
}

/* A key: 32 hex digits; absent or empty keeps the stored one. */
static void key_field(const cJSON *root, const char *name, uint8_t *key, const char **why)
{
    const cJSON *j = cJSON_GetObjectItem(root, name);
    if (cJSON_IsString(j) && j->valuestring[0] != '\0' && !ws_settings_parse_hex(j->valuestring, key, 16)) {
        *why = "the keys: 32 hex digits each";
    }
}

static esp_err_t ws_settings_api_post(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return require_auth(req);
    }
    char body[512];
    if (req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too long");
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    body[got] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not JSON");
    }

    ws_settings_t cfg;
    ws_settings_get(&cfg);
    const char *why = NULL;
    const cJSON *j;
    if (cJSON_IsBool(j = cJSON_GetObjectItem(root, "link_on"))) {
        cfg.link_on = cJSON_IsTrue(j);
    }
    if (cJSON_IsString(j = cJSON_GetObjectItem(root, "mac"))) {
        if (j->valuestring[0] == '\0') {
            memset(cfg.mac, 0, sizeof(cfg.mac));
        } else if (!ws_settings_parse_hex(j->valuestring, cfg.mac, sizeof(cfg.mac))) {
            why = "the MAC: six hex pairs, like aa:bb:cc:dd:ee:ff";
        }
    }
    key_field(root, "pmk", cfg.pmk, &why);
    key_field(root, "lmk", cfg.lmk, &why);
    if (cJSON_IsNumber(j = cJSON_GetObjectItem(root, "interval_s"))) {
        const double s = j->valuedouble;
        if (s < 0 || s > 65535) {
            why = "the interval must be 3 minutes to 6 hours";
        } else {
            cfg.interval_s = (uint16_t)lround(s);
        }
    }
    if (cJSON_IsBool(j = cJSON_GetObjectItem(root, "indoor_on"))) {
        cfg.indoor_on = cJSON_IsTrue(j);
    }
    scaled(root, "cal_out_t", 100, &cfg.cal_out_t, &why);
    scaled(root, "cal_out_rh", 100, &cfg.cal_out_rh, &why);
    scaled(root, "cal_in_t", 100, &cfg.cal_in_t, &why);
    scaled(root, "cal_in_rh", 100, &cfg.cal_in_rh, &why);
    scaled(root, "bat_trim_pm", 1, &cfg.bat_trim_pm, &why);
    if (cJSON_IsNull(j = cJSON_GetObjectItem(root, "alt_override_m"))) {
        cfg.alt_override_m = WS_ALT_AUTO;
    } else {
        scaled(root, "alt_override_m", 1, &cfg.alt_override_m, &why);
    }
    cJSON_Delete(root);

    bool restart = false;
    if (why != NULL || ws_settings_set(&cfg, &restart, &why) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why != NULL ? why : "not saved");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, restart ? "{\"saved\":true,\"restart\":true}" : "{\"saved\":true,\"restart\":false}");
    if (restart) {
        ESP_LOGI(TAG, "the outdoor unit's link or pairing changed: restarting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    return ESP_OK;
}

void weather_http_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/outdoor", .method = HTTP_GET, .handler = outdoor_page_get },
        { .uri = "/api/outdoor", .method = HTTP_GET, .handler = outdoor_api_get },
        { .uri = "/api/outdoor/events", .method = HTTP_GET, .handler = outdoor_events_get },
        { .uri = "/api/readings", .method = HTTP_GET, .handler = readings_api_get },
        { .uri = "/api/readings/history", .method = HTTP_GET, .handler = history_get },
        { .uri = "/api/weather-station/settings", .method = HTTP_GET, .handler = ws_settings_api_get },
        { .uri = "/api/weather-station/settings", .method = HTTP_POST, .handler = ws_settings_api_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(server, &uris[i]) != ESP_OK) {
            ESP_LOGW(TAG, "%s not registered (max_uri_handlers too low?)", uris[i].uri);
        }
    }
}

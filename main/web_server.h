#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

/* Start HTTP server (port 80) + mDNS (http://canflight.local).
 * Endpoints: GET / (panel), GET /api/state (JSON), POST /ota (firmware). */
void web_server_start(void);

/* Publish a fresh JSON state snapshot (copied; caller keeps ownership). */
void web_state_publish(const char *json);

/* Prometheus scalars, pushed by flight_task each cycle (metrics never
 * parse the state JSON). */
void web_metrics_publish(int count, int unique, int max_alt_ft, double nearest_km);

/* True while somebody polled /api/state within the last 5 minutes; the
 * flight task skips building the big state JSON otherwise. */
bool web_state_wanted(void);

/* weather-station fork: the panel password check, shared with weather_http.c.
 * A handler starts with: if (!auth_ok(req)) return require_auth(req); */
bool auth_ok(httpd_req_t *req);
esp_err_t require_auth(httpd_req_t *req);

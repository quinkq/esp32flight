#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

/* weather-station fork: the weather station's pages and API on the panel's
 * server, registered after upstream's own handlers. */
void weather_http_register(httpd_handle_t server);

/* true once the panel's web server runs: upstream's web_server_start() calls
 * weather_http_register() only after httpd_start() succeeded (ota_guard.c) */
bool weather_http_server_up(void);

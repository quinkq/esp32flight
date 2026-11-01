#pragma once

#include "esp_http_server.h"

/* weather-station fork: the weather station's pages and API on the panel's
 * server, registered after upstream's own handlers. */
void weather_http_register(httpd_handle_t server);

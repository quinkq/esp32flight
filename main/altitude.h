#pragma once

/* The altitude of the display's home location, for pressure at sea level
 * (weather-station fork). */

#include <stdbool.h>
#include "ws_settings.h"

/* Once, after Wi-Fi has started: looks the altitude up in the background
 * when the fixed home location is new. */
void altitude_start(void);

/* The altitude to reduce pressure with: the override from the settings, or
 * the one looked up while it's for the current fixed home location. false:
 * none, so pressure stays as measured. */
bool altitude_m(const ws_settings_t *cfg, int *m);

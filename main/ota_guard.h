#pragma once

/* weather-station fork: signed updates and the new firmware's self-check
 * (ota_guard.c). Call once, right after settings_load(). */
void ota_guard_start(void);

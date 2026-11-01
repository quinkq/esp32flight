#pragma once

/* The display's side of the link to the outdoor unit (ws-link over
 * ESP-NOW). Call once after Wi-Fi has started. Does nothing unless it's
 * enabled and paired in menuconfig ("Outdoor unit link"). */
void outdoor_link_start(void);

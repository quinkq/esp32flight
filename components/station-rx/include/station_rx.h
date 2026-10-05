#pragma once

/* Receiver for the ws_alpha outdoor station (ws-link over ESP-NOW). Call
 * once after Wi-Fi has started. Does nothing unless it's enabled and paired
 * in menuconfig ("Weather station receiver"). */
void station_rx_start(void);

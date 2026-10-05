# Receiver for the ws_alpha outdoor station (weather-station fork). The
# display's main calls station_rx_start() once Wi-Fi is up.
idf_component_register(SRCS "station_rx.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES ws-link esp_wifi)

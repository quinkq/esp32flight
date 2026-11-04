# The display's side of the link to the outdoor unit (weather-station fork).
# The display's main calls outdoor_link_start() once Wi-Fi is up.
idf_component_register(SRCS "outdoor_health.c" "outdoor_link.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES ws-link esp_wifi esp_timer spiffs)

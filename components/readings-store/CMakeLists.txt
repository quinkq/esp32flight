# The weather station's readings (weather-station fork): ~23 days in PSRAM
# and in the "readings" flash partition. The display's main calls
# readings_store_init() once; outdoor-link feeds it.
idf_component_register(SRCS "readings_store.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ws-link
                       PRIV_REQUIRES esp_partition esp_timer)

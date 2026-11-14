# The weather station's own settings (weather-station fork), in their own NVS
# namespace beside upstream's. The display's main calls ws_settings_init().
idf_component_register(SRCS "ws_settings.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash)

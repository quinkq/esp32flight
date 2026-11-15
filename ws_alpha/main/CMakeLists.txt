idf_component_register(SRCS "main.c" "power.c" "battery.c" "sensors.c" "backlog.c" "config.c" "link.c" "ota.c"
                       REQUIRES ws-link esp_driver_gpio esp_driver_i2c esp_adc esp_timer
                                esp_app_format esp_event esp_wifi nvs_flash
                                app_update esp_http_client esp_netif mbedtls)

idf_component_register(SRCS "main.c" "power.c" "battery.c" "sensors.c"
                       REQUIRES ws-link esp_driver_gpio esp_driver_i2c esp_adc esp_timer)

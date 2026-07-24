idf_component_register(
    SRCS "pb_httpd.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server pb_heater pb_ntc pb_leds pb_policy pv_evlog json nvs_flash app_update
             mbedtls esp_wifi esp_timer esp_system
)

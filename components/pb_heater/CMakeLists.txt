idf_component_register(
    SRCS "pb_heater.c"
    INCLUDE_DIRS "include"
    REQUIRES pb_board pb_ntc driver esp_timer nvs_flash
)

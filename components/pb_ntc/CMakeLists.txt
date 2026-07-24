idf_component_register(
    SRCS "pb_ntc.c"
    INCLUDE_DIRS "include"
    REQUIRES pb_board esp_adc nvs_flash
)

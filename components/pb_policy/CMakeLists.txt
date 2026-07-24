idf_component_register(
    SRCS "pb_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES pb_heater pb_fan pb_ntc pb_leds pb_buttons pv_evlog esp_timer esp_system freertos nvs_flash
)

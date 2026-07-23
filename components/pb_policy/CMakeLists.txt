idf_component_register(
    SRCS "pb_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES pb_heater pb_fan pb_ntc pb_leds esp_timer esp_system freertos
)

idf_component_register(
    SRCS "pb_moonraker.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash json esp_websocket_client pb_evlog
)

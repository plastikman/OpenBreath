idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES pb_board pb_ntc pb_heater pb_fan pb_policy pb_leds pb_buttons pb_hil pb_httpd pb_portal
             pb_wifi pb_moonraker pb_evlog nvs_flash esp_wifi esp_netif mdns app_update
)

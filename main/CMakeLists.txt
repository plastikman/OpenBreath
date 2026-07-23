idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES pb_board pb_ntc pb_heater pb_fan pb_policy pb_leds pb_httpd pb_portal
             pv_wifi pv_moonraker nvs_flash esp_wifi esp_netif mdns app_update
)

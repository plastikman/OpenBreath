idf_component_register(
    SRCS "pb_portal.c" "pb_dns.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server pv_wifi pb_httpd nvs_flash esp_wifi lwip
)

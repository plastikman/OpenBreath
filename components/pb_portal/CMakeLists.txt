idf_component_register(
    SRCS "pb_portal.c" "pb_dns.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server pv_wifi pb_httpd nvs_flash esp_wifi lwip json
)

# The STA-mode dashboard is a real source file (www/app.html) that we gzip at
# build time and embed into flash rodata. It is served as a single response with
# Content-Encoding: gzip — one document + one SSE stream, lighter on the socket
# pool than the old multi-fetch C-string page. Regenerated whenever app.html
# changes.
find_program(GZIP_EXECUTABLE gzip REQUIRED)
set(APP_HTML "${COMPONENT_DIR}/www/app.html")
set(APP_HTML_GZ "${CMAKE_CURRENT_BINARY_DIR}/app.html.gz")
add_custom_command(
    OUTPUT "${APP_HTML_GZ}"
    # -9 max compression, -n omit name/timestamp so the embedded blob is reproducible.
    COMMAND "${GZIP_EXECUTABLE}" -9 -n -c "${APP_HTML}" > "${APP_HTML_GZ}"
    MAIN_DEPENDENCY "${APP_HTML}"
    COMMENT "Gzipping web UI (app.html -> app.html.gz)"
    VERBATIM
)
# target_add_binary_data understands generated inputs and emits the
# _binary_app_html_gz_start/_end symbols used by pb_portal.c.
target_add_binary_data(${COMPONENT_LIB} "${APP_HTML_GZ}" BINARY)

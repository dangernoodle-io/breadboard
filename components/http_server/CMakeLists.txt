idf_component_register(
    SRCS "src/http_server.c" "src/http_utils.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_config log_stream esp_http_server esp_wifi esp_netif
)

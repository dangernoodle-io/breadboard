# bb_serialize: REQUIRES -- bb_http_client.h is a public header that itself
# includes bb_serialize.h (bb_serialize_desc_t, B1-1041 per-session health),
# so bb_serialize must be public here too (mirrors bb_tcp_client's
# CMakeLists.txt). bb_clock_now_ms64() (src/bb_http_client_health.c's
# last_ok_ms source) lives in bb_core, already REQUIRES'd below.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_http_client/bb_http_client_espidf.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_http_client_health.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core bb_serialize
    PRIV_REQUIRES bb_log esp_http_client esp-tls bb_str)

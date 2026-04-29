idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ntp/bb_ntp.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core esp_netif lwip
    PRIV_REQUIRES bb_log
)

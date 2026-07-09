idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_partition/bb_partition.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_partition/bb_partition_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http_server
    PRIV_REQUIRES bb_log esp_partition app_update bb_http bb_json bb_str)

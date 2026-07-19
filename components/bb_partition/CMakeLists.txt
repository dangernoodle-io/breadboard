idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_partition/bb_partition.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_partition/bb_partition_routes.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_partition_serialize.c"
    INCLUDE_DIRS "include"
    # bb_serialize: REQUIRES -- bb_partition_serialize.h is a public header
    # that itself includes bb_serialize.h (bb_serialize_field_t), same
    # public-header-leak rationale as bb_meminfo's own bb_serialize REQUIRES.
    REQUIRES bb_core bb_http_server bb_serialize
    PRIV_REQUIRES bb_log esp_partition app_update bb_http bb_json bb_str)

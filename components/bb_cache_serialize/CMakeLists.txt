idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_cache_serialize/bb_cache_serialize.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_cache bb_serialize bb_serialize_json
    PRIV_REQUIRES bb_log)

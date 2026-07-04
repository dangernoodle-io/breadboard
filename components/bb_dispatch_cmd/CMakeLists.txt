idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_dispatch_cmd/bb_dispatch_cmd.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_json
    PRIV_REQUIRES bb_log
)

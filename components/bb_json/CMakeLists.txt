idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_json/bb_json_cjson.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_json.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES bb_log json
)

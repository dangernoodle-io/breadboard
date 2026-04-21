idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_json/bb_json_cjson.c"
    INCLUDE_DIRS "include"
    REQUIRES json
)

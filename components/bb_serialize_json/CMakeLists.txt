idf_component_register(
    SRCS "src/bb_serialize_json.c" "src/bb_serialize_json_scan.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_serialize bb_core
)

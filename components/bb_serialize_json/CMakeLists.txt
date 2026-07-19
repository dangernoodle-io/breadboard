idf_component_register(
    SRCS "src/bb_serialize_json.c" "src/bb_serialize_json_scan.c" "src/bb_serialize_json_tok.c"
         "src/bb_serialize_json_format.c" "src/bb_serialize_json_populate.c" "src/bb_serialize_json_parse.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_serialize bb_core
    PRIV_REQUIRES bb_num bb_mem_arena
)

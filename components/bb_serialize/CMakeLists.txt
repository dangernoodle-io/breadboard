idf_component_register(
    SRCS "src/bb_serialize_walk.c" "src/bb_serialize_populate.c" "src/bb_serialize_format.c"
         "src/bb_serialize_compose.c" "src/bb_serialize_query.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_registry bb_log
)

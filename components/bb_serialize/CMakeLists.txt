idf_component_register(
    SRCS "src/bb_serialize_walk.c" "src/bb_serialize_format.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_registry bb_log
)

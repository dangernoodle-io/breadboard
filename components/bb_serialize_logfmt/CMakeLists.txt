idf_component_register(
    SRCS "src/bb_serialize_logfmt.c" "src/bb_serialize_logfmt_format.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_serialize bb_core
    PRIV_REQUIRES bb_num
)

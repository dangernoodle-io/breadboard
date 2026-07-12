idf_component_register(
    SRCS "bb_lifecycle.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log)

idf_component_register(
    SRCS "bb_lifecycle.c" "bb_lifecycle_async.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_log bb_bqueue bb_task)

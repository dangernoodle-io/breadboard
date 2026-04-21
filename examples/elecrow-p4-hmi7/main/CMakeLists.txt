idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES bb_nv bb_log bb_display nvs_flash)

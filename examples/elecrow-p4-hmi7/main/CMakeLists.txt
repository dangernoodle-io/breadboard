idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES bb_nv bb_log bb_display bb_prov_default_form nvs_flash)

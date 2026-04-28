idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES bb_nv bb_log bb_wifi bb_http bb_mdns bb_registry bb_ota_pull bb_ota_push bb_info bb_board bb_manifest bb_ota_validator bb_system bb_openapi)

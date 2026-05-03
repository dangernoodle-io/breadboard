set(SMOKE_REQUIRES bb_nv bb_log bb_wifi bb_http bb_mdns bb_registry bb_ota_pull bb_ota_push bb_info bb_board bb_manifest bb_ota_validator bb_system bb_openapi)

if(FIRMWARE_BOARD STREQUAL "elecrow_p4_hmi7")
    list(APPEND SMOKE_REQUIRES bb_display bb_display_ek79007)
elseif(FIRMWARE_BOARD STREQUAL "lilygo_t_dongle_s3")
    list(APPEND SMOKE_REQUIRES bb_display bb_display_st77xx)
endif()

idf_component_register(
    SRCS "entry_espidf.c" "smoke_app.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES ${SMOKE_REQUIRES})

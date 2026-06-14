include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/bb_embed.cmake")

bb_embed_site(
    OUT_SRCS _site_srcs
    TABLE demo_site
    DIST_DIR "${CMAKE_CURRENT_LIST_DIR}/demo_site"
)

set(SMOKE_REQUIRES bb_nv bb_log bb_wifi bb_http bb_mdns bb_registry bb_ota_pull bb_ota_push bb_ota_boot bb_info bb_board bb_manifest bb_ota_validator bb_system bb_openapi bb_led bb_led_info bb_ntp bb_ntp_info bb_led_gpio bb_led_pwm bb_led_apa102 bb_led_anim bb_button bb_button_gpio bb_button_events bb_event bb_event_ring bb_event_ring_espidf bb_event_routes bb_event_routes_espidf bb_http_client bb_release_manifest bb_update_check bb_temp bb_power bb_power_routes bb_fan bb_fan_routes bb_thermal bb_tls_creds bb_mqtt bb_pub bb_sink_mqtt bb_pub_fan bb_pub_power bb_pub_thermal bb_pub_info bb_pub_wifi bb_pub_rtos)

if(FIRMWARE_BOARD STREQUAL "elecrow_p4_hmi7")
    list(APPEND SMOKE_REQUIRES bb_display bb_display_ek79007 bb_display_info)
elseif(FIRMWARE_BOARD STREQUAL "lilygo_t_dongle_s3")
    list(APPEND SMOKE_REQUIRES bb_display bb_display_st77xx bb_display_info)
endif()

# Wire bb_power_tps546 and bb_fan_emc2101 on esp32 and tdongle boards to prove
# the espidf backends compile and link (no real hardware required).
if(FIRMWARE_BOARD STREQUAL "esp32_wroom_32" OR FIRMWARE_BOARD STREQUAL "lilygo_t_dongle_s3")
    list(APPEND SMOKE_REQUIRES bb_power_tps546 bb_fan_emc2101)
endif()

idf_component_register(
    SRCS "entry_espidf.c" "smoke_app.c" ${_site_srcs}
    INCLUDE_DIRS "." "../include"
    REQUIRES ${SMOKE_REQUIRES})

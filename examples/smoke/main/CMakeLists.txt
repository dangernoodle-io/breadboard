include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/bbtool.cmake")

bb_embed_site(
    OUT_SRCS _site_srcs
    TABLE demo_site
    DIST_DIR "${CMAKE_CURRENT_LIST_DIR}/demo_site"
)

# bb_task is linked here (unused -- no call site migrated yet, per the
# task-registry unification PR2b/PR3 split) solely to prove the espidf
# creation shell compiles under -Werror, mirroring the bb_power_tps546/
# bb_fan_emc2101 "prove it links" precedent below.
# bb_log_event / bb_log_http were carved out of bb_log (KB #708/#704) --
# smoke previously got the "log" bb_event topic + GET/POST /api/log/level
# routes for free via bb_log's own Kconfig-gated PRIV_REQUIRES; now that
# bb_log's base component has zero bb_http_server/bb_event deps, smoke must
# REQUIRES the two sink components explicitly to keep the same functional
# coverage (bb_sink_ws below subscribes to the "log" topic bb_log_event
# registers).
# bb_config is linked here (unused -- no call site migrated yet) solely to
# prove the component compiles under -Werror on espidf, mirroring the
# bb_task/bb_dispatch_cmd "prove it links" precedent.
# bb_storage_nvs backs storage_typed_selftest.c's KB 791 Option A target
# proof (bb_nv <-> bb_config STR byte-identity round trip over the native
# NVS typed-entry path) -- see that file's header comment.
# bb_wifi_creds + bb_settings compose the wifi-creds provider seam: entry_espidf.c
# injects bb_settings' default provider into bb_wifi before bb_init_init_early()
# runs the EARLY-tier bb_wifi_autoinit.
# bb_wifi_http (PR1, KB 781) is the opt-in STA /api/wifi + /api/scan route
# bundle extracted from bb_wifi's STA core -- composed here to keep today's
# routes working; a minimal STA-only image would omit it.
set(SMOKE_REQUIRES bb_nv bb_log bb_log_event bb_log_http bb_wifi bb_wifi_http bb_wifi_creds bb_settings bb_http bb_http_server bb_mdns bb_mdns_cache bb_init bb_ota_pull bb_ota_push bb_ota_boot bb_info bb_board bb_manifest bb_ota_validator bb_system bb_openapi bb_led bb_led_info bb_ntp bb_ntp_info bb_led_gpio bb_led_pwm bb_led_apa102 bb_led_anim bb_button bb_button_gpio bb_button_events bb_event bb_event_ring bb_event_ring_espidf bb_event_routes bb_event_routes_espidf bb_ring_espidf bb_ring_diag bb_http_client bb_release_manifest bb_ota_check bb_temp bb_power bb_fan bb_sensors bb_tls_creds bb_mqtt_client bb_pub bb_sink_mqtt bb_pub_fan bb_pub_power bb_pub_thermal bb_pub_info bb_pub_wifi bb_pub_rtos bb_ws_server bb_sink_ws bb_registry bb_partition bb_task_registry bb_task bb_net_health bb_mem_arena bb_pool bb_udp_frame bb_udp_client bb_sink_udp bb_dispatch_cmd bb_meminfo bb_timer bb_config bb_storage_nvs)

# bb_sink_display is linked here (unused -- no call site migrated yet) solely
# to prove the espidf glue (platform/espidf/bb_sink_display/bb_sink_display.c)
# compiles under -Werror on a board that already links bb_display, mirroring
# the bb_task "prove it links" precedent below.
if(FIRMWARE_BOARD STREQUAL "elecrow_p4_hmi7")
    list(APPEND SMOKE_REQUIRES bb_display bb_display_ek79007 bb_display_info bb_sink_display)
elseif(FIRMWARE_BOARD STREQUAL "lilygo_t_dongle_s3")
    list(APPEND SMOKE_REQUIRES bb_display bb_display_st77xx bb_display_info bb_sink_display)
endif()

# Wire bb_power_tps546 and bb_fan_emc2101 on esp32 and tdongle boards to prove
# the espidf backends compile and link (no real hardware required).
if(FIRMWARE_BOARD STREQUAL "esp32_wroom_32" OR FIRMWARE_BOARD STREQUAL "lilygo_t_dongle_s3")
    list(APPEND SMOKE_REQUIRES bb_power_tps546 bb_fan_emc2101)
endif()

idf_component_register(
    SRCS "entry_espidf.c" "smoke_app.c" "storage_typed_selftest.c" ${_site_srcs}
    INCLUDE_DIRS "." "../include"
    REQUIRES ${SMOKE_REQUIRES})

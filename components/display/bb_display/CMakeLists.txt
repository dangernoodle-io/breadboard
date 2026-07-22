set(SRCS
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_display.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/bb_display_info_wire.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../../platform/espidf/bb_display/bb_display_info.c")
# bb_serialize: PUBLIC -- include/bb_display_info_wire.h (the
# "health.display" wire descriptor, B1-1045 PR-2) includes bb_serialize.h.
set(_display_reqs bb_core bb_http_server bb_serialize)
# B1-893: bb_cache/bb_openapi/bb_str re-homed here from the deleted
# bb_display_info satellite (health.display cache registration -- see
# src/bb_display_info_event_priv.h banner). bb_data (B1-1045): PRIV --
# bb_data_touch("health.display"); also backs bb_display_info_bind()
# (B1-1146a, src/bb_display_info_wire.c).
# bb_json: DROPPED (B1-1146a) -- the last bb_json use in this component
# (bb_display_serialize(), the legacy bb_cache serializer,
# src/bb_display_info_event_common.c, now deleted) is gone; health.display's
# REST exposure is being rehomed to system.display under bb_system's diag
# endpoint (B1-1150), not this component.
# bb_nv -> bb_settings: display_enabled moved to bb_settings (B1-750).
set(_display_priv_reqs bb_log bb_cache bb_data bb_openapi bb_settings bb_str)

if(CONFIG_BB_DISPLAY_FONT_8X16)
    list(APPEND SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_display_font_8x16.c")
endif()
if(CONFIG_BB_DISPLAY_FONT_6X12)
    list(APPEND SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_display_font_6x12.c")
endif()
if(CONFIG_BB_DISPLAY_FONT_5X8)
    list(APPEND SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_display_font_5x8.c")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include"
    # "src" carries bb_display_info_event_priv.h -- the private header
    # platform/espidf/bb_display/bb_display_info.c needs (B1-893 re-home).
    PRIV_INCLUDE_DIRS "src"
    REQUIRES ${_display_reqs}
    PRIV_REQUIRES ${_display_priv_reqs})

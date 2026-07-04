if(ESP_PLATFORM)
    set(BB_SINK_WS_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_sink_ws/bb_sink_ws.c")
else()
    set(BB_SINK_WS_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_sink_ws/bb_sink_ws.c")
endif()

idf_component_register(
    SRCS ${BB_SINK_WS_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_pub bb_websocket
    PRIV_REQUIRES bb_log bb_event bb_json)

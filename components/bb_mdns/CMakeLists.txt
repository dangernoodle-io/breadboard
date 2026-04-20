idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_mdns/bb_mdns.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__mdns esp_wifi esp_system esp_app_format log_stream board bb_wifi
)

# Ensure managed component is available
if(DEFINED IDF_COMPONENT_MANAGER AND IDF_COMPONENT_MANAGER)
    # Will be fetched via idf_component.yml
endif()

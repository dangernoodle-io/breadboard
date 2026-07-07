idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_fan_routes/bb_fan_routes.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_fan bb_core bb_json
    PRIV_REQUIRES bb_log bb_init bb_http_server)

if(CONFIG_BB_FAN_ROUTES_AUTOREGISTER)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
    bb_init_force_register(${COMPONENT_LIB} bb_fan_routes)
endif()

# test/test_host/test_emc2101_decode.c exercises bb_fan_emc2101's pure decode
# header directly; bb_fan_emc2101 itself is an ESP-IDF-only I2C driver (no
# platform/host backend) so it's never a requested scaffold component, but
# its decode header must still be on the host test include path.
# bbtool-scaffold-hint: include=components/bb_fan_emc2101/include

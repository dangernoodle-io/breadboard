set(BB_FAN_SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_fan/bb_fan.c")

set(BB_FAN_PRIV_REQUIRES bb_log)
if(CONFIG_BB_FAN_AUTOFAN)
    list(APPEND BB_FAN_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_fan_pid.c")
    if(IDF_TARGET)
        # ESP-IDF platform: add the esp_timer-backed clock shim
        list(APPEND BB_FAN_SRCS
            "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_fan/bb_fan_clock_espidf.c")
        list(APPEND BB_FAN_PRIV_REQUIRES esp_timer)
    endif()
endif()

idf_component_register(
    SRCS ${BB_FAN_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES ${BB_FAN_PRIV_REQUIRES})

# include/ exposes bb_fan_driver.h (semi-private driver vtable interface).
# Fan driver components (e.g. bb_fan_emc2101) REQUIRES bb_fan to access it.

# test/test_host/test_emc2101_decode.c exercises bb_fan_emc2101's pure decode
# header directly; bb_fan_emc2101 itself is an ESP-IDF-only I2C driver (no
# platform/host backend) so it's never a requested scaffold component, but
# its decode header must still be on the host test include path. Hosted here
# (not on bb_fan_emc2101 itself) because bb_fan is the component actually
# reachable from [capability.core]'s native test graph.
# bbtool-scaffold-hint: include=components/bb_fan_emc2101/include

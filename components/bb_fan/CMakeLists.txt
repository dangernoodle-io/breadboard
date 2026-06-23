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
    REQUIRES bb_core bb_json
    PRIV_REQUIRES ${BB_FAN_PRIV_REQUIRES})

# include/ exposes bb_fan_driver.h (semi-private driver vtable interface).
# Fan driver components (e.g. bb_fan_emc2101) REQUIRES bb_fan to access it.

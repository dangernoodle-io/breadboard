set(BB_I2C_SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_i2c/bb_i2c.c")
set(BB_I2C_PRIV_REQUIRES bb_log)

if(IDF_TARGET)
    # ESP-IDF platform: swap in the real driver backend
    set(BB_I2C_SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_i2c/bb_i2c.c")
    list(APPEND BB_I2C_PRIV_REQUIRES esp_driver_i2c)
endif()

idf_component_register(
    SRCS ${BB_I2C_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES ${BB_I2C_PRIV_REQUIRES})

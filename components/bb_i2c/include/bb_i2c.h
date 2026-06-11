// bb_i2c — portable I2C bus + device abstraction.
// No ESP-IDF types in this header; consumers never touch esp_driver_i2c.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles. Implementations live in platform/espidf/ and platform/host/.
typedef struct bb_i2c_bus_s *bb_i2c_bus_t;
typedef struct bb_i2c_dev_s *bb_i2c_dev_t;

// Bus configuration. port/sda_gpio/scl_gpio are platform-specific integers
// (GPIO numbers on ESP-IDF; ignored on host).
typedef struct {
    int      port;
    int      sda_gpio;
    int      scl_gpio;
    uint32_t clk_hz;          // bus clock frequency; must be > 0
    bool     internal_pullup; // enable internal pull-ups (ESP-IDF)
} bb_i2c_bus_config_t;

// Create a new I2C bus instance. Returns BB_ERR_INVALID_ARG if cfg or out is NULL,
// or clk_hz is 0.
bb_err_t bb_i2c_bus_create(const bb_i2c_bus_config_t *cfg, bb_i2c_bus_t *out);

// Destroy the bus. All devices on this bus should be removed first.
void bb_i2c_bus_delete(bb_i2c_bus_t bus);

// Add a device to the bus. addr7 is the 7-bit device address.
// speed_hz 0 = inherit bus default. Returns BB_ERR_INVALID_ARG if bus or out is NULL.
bb_err_t bb_i2c_dev_add(bb_i2c_bus_t bus, uint8_t addr7, uint32_t speed_hz,
                         bb_i2c_dev_t *out);

// Remove a device. dev may be NULL (no-op).
void bb_i2c_dev_remove(bb_i2c_dev_t dev);

// Write len bytes from buf to the device.
// Returns BB_ERR_INVALID_ARG if dev or buf is NULL, or len is 0.
bb_err_t bb_i2c_dev_write(bb_i2c_dev_t dev, const uint8_t *buf, size_t len);

// Read len bytes from the device into buf.
// Returns BB_ERR_INVALID_ARG if dev or buf is NULL, or len is 0.
bb_err_t bb_i2c_dev_read(bb_i2c_dev_t dev, uint8_t *buf, size_t len);

// Write wlen bytes then read rlen bytes in a single transaction.
// Returns BB_ERR_INVALID_ARG if dev, w, or r is NULL, or either len is 0.
bb_err_t bb_i2c_dev_write_read(bb_i2c_dev_t dev,
                                const uint8_t *w, size_t wlen,
                                uint8_t       *r, size_t rlen);

// Convenience: write register address reg then read one byte.
// Returns BB_ERR_INVALID_ARG if dev or val is NULL.
bb_err_t bb_i2c_dev_read_reg8(bb_i2c_dev_t dev, uint8_t reg, uint8_t *val);

// Convenience: write [reg, val] in one transaction.
// Returns BB_ERR_INVALID_ARG if dev is NULL.
bb_err_t bb_i2c_dev_write_reg8(bb_i2c_dev_t dev, uint8_t reg, uint8_t val);

#ifdef __cplusplus
}
#endif

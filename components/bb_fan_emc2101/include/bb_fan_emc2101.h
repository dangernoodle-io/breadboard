// bb_fan_emc2101 — ESP-IDF fan/temperature backend for the EMC2101.
#pragma once
#include "bb_fan.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"

typedef struct {
    i2c_master_bus_handle_t bus;
    uint8_t addr;      // 7-bit I2C address
    uint8_t ideality;  // ideality factor register value (0 = skip write)
    uint8_t beta;      // beta compensation register value (0 = skip write)
} bb_fan_emc2101_cfg_t;

// Open an EMC2101 backend handle.
// Allocates instance state, runs the EMC2101 init sequence (config register,
// fan config, optional ideality/beta writes, failsafe 100% duty), registers
// the vtable, and calls bb_fan_handle_create.
bb_err_t bb_fan_emc2101_open(const bb_fan_emc2101_cfg_t *cfg,
                              bb_fan_handle_t *out);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif

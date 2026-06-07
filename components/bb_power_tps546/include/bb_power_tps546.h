// bb_power_tps546 — ESP-IDF backend for the TPS546029 voltage regulator.
#pragma once
#include "bb_power.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"

typedef struct {
    i2c_master_bus_handle_t bus;
    uint8_t  addr;            // 7-bit I2C address
    uint16_t target_mv;       // initial output voltage in millivolts
    uint32_t switch_freq_khz; // switching frequency (0 = use default 650 kHz)
    uint8_t  oc_limit_a;      // overcurrent fault limit in amps (0 = use default 30 A)
    uint8_t  oc_response;     // OC fault response byte (0 = use default 0xC0)
} bb_power_tps546_cfg_t;

// Open a TPS546 backend handle.
// Allocates state, runs the TPS546 init sequence, registers the vtable,
// and calls bb_power_handle_create.
bb_err_t bb_power_tps546_open(const bb_power_tps546_cfg_t *cfg,
                               bb_power_handle_t *out);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif

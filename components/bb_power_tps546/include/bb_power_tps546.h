// bb_power_tps546 — ESP-IDF backend for the TPS546029 voltage regulator.
#pragma once
#include "bb_power.h"
#include "tps546_decode.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// PMBus register addresses (from AxeOS pmbus_commands.h — verbatim)
// ---------------------------------------------------------------------------
#define BB_PMBUS_OPERATION              0x01u
#define BB_PMBUS_ON_OFF_CONFIG          0x02u
#define BB_PMBUS_CLEAR_FAULTS           0x03u
#define BB_PMBUS_PHASE                  0x04u
#define BB_PMBUS_VOUT_MODE              0x20u
#define BB_PMBUS_VOUT_COMMAND           0x21u
#define BB_PMBUS_VOUT_MAX               0x24u
#define BB_PMBUS_VOUT_MARGIN_HIGH       0x25u
#define BB_PMBUS_VOUT_MARGIN_LOW        0x26u
#define BB_PMBUS_VOUT_SCALE_LOOP        0x29u
#define BB_PMBUS_VOUT_MIN               0x2Bu
#define BB_PMBUS_FREQUENCY_SWITCH       0x33u
#define BB_PMBUS_VIN_ON                 0x35u
#define BB_PMBUS_VIN_OFF                0x36u
#define BB_PMBUS_VOUT_OV_FAULT_LIMIT    0x40u
#define BB_PMBUS_VOUT_OV_WARN_LIMIT     0x42u
#define BB_PMBUS_VOUT_UV_WARN_LIMIT     0x43u
#define BB_PMBUS_VOUT_UV_FAULT_LIMIT    0x44u
#define BB_PMBUS_VOUT_UV_FAULT_RESPONSE 0x45u
#define BB_PMBUS_IOUT_OC_FAULT_LIMIT    0x46u
#define BB_PMBUS_IOUT_OC_FAULT_RESPONSE 0x47u
#define BB_PMBUS_IOUT_OC_WARN_LIMIT     0x4Au
#define BB_PMBUS_OT_FAULT_LIMIT         0x4Fu
#define BB_PMBUS_OT_FAULT_RESPONSE      0x50u
#define BB_PMBUS_OT_WARN_LIMIT          0x51u
#define BB_PMBUS_VIN_OV_FAULT_LIMIT     0x55u
#define BB_PMBUS_VIN_OV_FAULT_RESPONSE  0x56u
#define BB_PMBUS_VIN_UV_WARN_LIMIT      0x58u
#define BB_PMBUS_TON_DELAY              0x60u
#define BB_PMBUS_TON_RISE               0x61u
#define BB_PMBUS_TON_MAX_FAULT_LIMIT    0x62u
#define BB_PMBUS_TON_MAX_FAULT_RESPONSE 0x63u
#define BB_PMBUS_READ_VIN               0x88u
#define BB_PMBUS_READ_VOUT              0x8Bu
#define BB_PMBUS_READ_IOUT              0x8Cu
#define BB_PMBUS_READ_TEMPERATURE_1     0x8Du
#define BB_PMBUS_COMPENSATION_CONFIG    0xB1u
#define BB_PMBUS_SYNC_CONFIG            0xE4u
#define BB_PMBUS_STACK_CONFIG           0xECu

// ---------------------------------------------------------------------------
// Protection / soft-start configuration sub-struct.
// All fields default to 0 (zero-init). A zero value means "skip this write"
// (mirrors AxeOS behaviour). Existing callers that don't populate this struct
// get no behaviour change — every new write is guarded by a non-zero check.
//
// VOUT OV/UV factors are multiplied by (target_mv/1000) at encode time to
// derive absolute voltage thresholds, e.g. vout_ov_fault_factor=1.25 with a
// 1150 mV target → OV fault limit = 1.4375 V.
// ---------------------------------------------------------------------------
typedef struct {
    // VIN protection
    float    vin_on_v;               // VIN turn-on threshold (V); 0 = skip
    float    vin_off_v;              // VIN turn-off threshold (V); 0 = skip
    float    vin_uv_warn_v;          // VIN UV warn limit (V); 0 = skip
    float    vin_ov_fault_v;         // VIN OV fault limit (V); 0 = skip
    uint8_t  vin_ov_fault_response;  // VIN OV fault response byte; 0 = skip

    // VOUT scale / absolute clamps
    float    vout_scale_loop;        // VOUT_SCALE_LOOP factor (SLINEAR11); 0 = skip
    float    vout_max_v;             // VOUT_MAX absolute (V, ULINEAR16); 0 = skip
    float    vout_min_v;             // VOUT_MIN absolute (V, ULINEAR16); 0 = skip

    // VOUT OV/UV proportional limit factors (multiplied by target_V at encode time)
    float    vout_ov_fault_factor;   // OV fault limit  = factor × target_V; 0 = skip
    float    vout_ov_warn_factor;    // OV warn limit   = factor × target_V; 0 = skip
    float    vout_margin_high;       // MARGIN_HIGH     = factor × target_V; 0 = skip
    float    vout_margin_low;        // MARGIN_LOW      = factor × target_V; 0 = skip
    float    vout_uv_warn_factor;    // UV warn limit   = factor × target_V; 0 = skip
    float    vout_uv_fault_factor;   // UV fault limit  = factor × target_V; 0 = skip
    // VOUT UV fault response byte (0 = skip → chip default latch-off).
    // 0xBF = shut down + continuous hiccup restart with max inter-retry delay,
    // so a transient undervoltage self-recovers instead of latching.
    uint8_t  vout_uv_fault_response;

    // IOUT (OC warn; OC fault/response come from the outer cfg fields)
    float    iout_oc_warn_a;         // IOUT OC warn limit (A, SLINEAR11); 0 = skip

    // Over-temperature
    int      ot_warn_c;              // OT warn limit (°C, SLINEAR11 int); 0 = skip
    int      ot_fault_c;             // OT fault limit (°C, SLINEAR11 int); 0 = skip
    uint8_t  ot_fault_response;      // OT fault response byte; 0 = skip

    // Soft-start / timing (ms, encoded as SLINEAR11 int)
    int      ton_delay_ms;           // TON_DELAY (ms); 0 = skip
    int      ton_rise_ms;            // TON_RISE (ms); 0 = skip
    int      ton_max_fault_ms;       // TON_MAX_FAULT_LIMIT (ms); 0 = skip
    uint8_t  ton_max_fault_response; // TON_MAX_FAULT_RESPONSE byte; 0 = skip

    // Phase / stack / sync topology
    uint8_t  on_off_config;          // ON_OFF_CONFIG byte; 0 = skip
    uint16_t stack_config;           // STACK_CONFIG word; 0 = skip
    uint8_t  sync_config;            // SYNC_CONFIG byte; 0 = skip
    uint8_t  phase;                  // PHASE byte; 0 = skip

    // Compensation (5-byte block). Written only when any byte is non-zero.
    uint8_t  compensation_config[5];
} bb_power_tps546_protect_t;

// ---------------------------------------------------------------------------
// Write-program entry — describes one PMBus write.
// ---------------------------------------------------------------------------
typedef enum {
    BB_TPS546_W_BYTE   = 1,
    BB_TPS546_W_WORD   = 2,
    BB_TPS546_W_BLOCK5 = 5,
} bb_tps546_width_t;

typedef struct {
    uint8_t           reg;
    bb_tps546_width_t width;
    uint16_t          word;      // used for BYTE and WORD writes
    uint8_t           block[5]; // used for BLOCK5 writes
    bool              essential; // if false, a write error is warned but non-fatal
} bb_tps546_write_t;

// ---------------------------------------------------------------------------
// Main configuration struct.
// Fields present on both host and ESP-IDF.
// The `bus` field is only compiled on ESP-IDF (I2C master bus handle).
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"
typedef struct {
    i2c_master_bus_handle_t   bus;            // I2C master bus (ESP-IDF only)
    uint8_t                   addr;           // 7-bit I2C address
    uint16_t                  target_mv;      // initial output voltage in millivolts
    uint32_t                  switch_freq_khz;// switching frequency (0 = default 650 kHz)
    uint8_t                   oc_limit_a;     // OC fault limit in amps (0 = default 30 A)
    uint8_t                   oc_response;    // OC fault response byte (0 = default 0xC0)
    bb_power_tps546_protect_t protect;        // full protection config (zero-init = skip all)
} bb_power_tps546_cfg_t;

// Open a TPS546 backend handle.
// Allocates state, runs the TPS546 init sequence, registers the vtable,
// and calls bb_power_handle_create.
bb_err_t bb_power_tps546_open(const bb_power_tps546_cfg_t *cfg,
                               bb_power_handle_t *out);

#else /* !ESP_PLATFORM — host test build */

typedef struct {
    uint8_t                   addr;
    uint16_t                  target_mv;
    uint32_t                  switch_freq_khz;
    uint8_t                   oc_limit_a;
    uint8_t                   oc_response;
    bb_power_tps546_protect_t protect;
} bb_power_tps546_cfg_t;

#endif /* ESP_PLATFORM */

// ---------------------------------------------------------------------------
// Build the ordered PMBus init program for a given config + VOUT_MODE exp.
// Pure function — no I2C dependency, host-testable.
// Returns the number of write entries placed in `out`, or -1 on overflow.
//
// Write order matches AxeOS write_entire_config() exactly:
//   ON_OFF_CONFIG, STACK_CONFIG, SYNC_CONFIG, PHASE,
//   FREQUENCY_SWITCH, [COMPENSATION_CONFIG if non-zero],
//   [VIN_UV_WARN_LIMIT if >0], VIN_ON, VIN_OFF,
//   VIN_OV_FAULT_LIMIT, VIN_OV_FAULT_RESPONSE,
//   VOUT_SCALE_LOOP, VOUT_COMMAND, VOUT_MAX, VOUT_MIN,
//   VOUT_OV_FAULT_LIMIT, VOUT_OV_WARN_LIMIT,
//   VOUT_MARGIN_HIGH, VOUT_MARGIN_LOW,
//   VOUT_UV_WARN_LIMIT, VOUT_UV_FAULT_LIMIT,
//   IOUT_OC_WARN_LIMIT, IOUT_OC_FAULT_LIMIT, IOUT_OC_FAULT_RESPONSE,
//   OT_WARN_LIMIT, OT_FAULT_LIMIT, OT_FAULT_RESPONSE,
//   TON_DELAY, TON_RISE, [TON_MAX_FAULT_LIMIT], [TON_MAX_FAULT_RESPONSE]
// ---------------------------------------------------------------------------
int bb_power_tps546_build_init_program(
        const bb_power_tps546_cfg_t *cfg,
        int8_t vout_exp,
        bb_tps546_write_t *out,
        int max);

#ifdef __cplusplus
}
#endif

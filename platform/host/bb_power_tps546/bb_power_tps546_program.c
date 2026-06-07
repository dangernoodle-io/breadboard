// bb_power_tps546_program.c — pure PMBus init-program builder.
// No I2C, no ESP-IDF dependency. Compiled on both host and ESP-IDF.
// The write order matches AxeOS write_entire_config() exactly.
#include "bb_power_tps546.h"
#include "tps546_decode.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static int emit_byte_ex(bb_tps546_write_t *out, int max, int *n,
                        uint8_t reg, uint8_t val, bool essential)
{
    if (*n >= max) return -1;
    out[*n].reg       = reg;
    out[*n].width     = BB_TPS546_W_BYTE;
    out[*n].word      = val;
    out[*n].essential = essential;
    memset(out[*n].block, 0, 5);
    (*n)++;
    return 0;
}

static int emit_word_ex(bb_tps546_write_t *out, int max, int *n,
                        uint8_t reg, uint16_t val, bool essential)
{
    if (*n >= max) return -1;
    out[*n].reg       = reg;
    out[*n].width     = BB_TPS546_W_WORD;
    out[*n].word      = val;
    out[*n].essential = essential;
    memset(out[*n].block, 0, 5);
    (*n)++;
    return 0;
}

static int emit_block5_ex(bb_tps546_write_t *out, int max, int *n,
                          uint8_t reg, const uint8_t *data, bool essential)
{
    if (*n >= max) return -1;
    out[*n].reg       = reg;
    out[*n].width     = BB_TPS546_W_BLOCK5;
    out[*n].word      = 0;
    out[*n].essential = essential;
    memcpy(out[*n].block, data, 5);
    (*n)++;
    return 0;
}

// Convenience wrappers — essential=false (protection writes, best-effort)
static int emit_byte(bb_tps546_write_t *out, int max, int *n,
                     uint8_t reg, uint8_t val)
{
    return emit_byte_ex(out, max, n, reg, val, false);
}

static int emit_word(bb_tps546_write_t *out, int max, int *n,
                     uint8_t reg, uint16_t val)
{
    return emit_word_ex(out, max, n, reg, val, false);
}

static int emit_block5(bb_tps546_write_t *out, int max, int *n,
                       uint8_t reg, const uint8_t *data)
{
    return emit_block5_ex(out, max, n, reg, data, false);
}

// Convenience: emit if the int guard value is non-zero.
#define EMIT_BYTE_IF(reg, val) \
    do { if ((val) && emit_byte(out, max, &n, (reg), (uint8_t)(val)) < 0) return -1; } while (0)

#define EMIT_WORD_IF(reg, val) \
    do { if ((val) && emit_word(out, max, &n, (reg), (uint16_t)(val)) < 0) return -1; } while (0)

// For float guards: non-zero float.
#define EMIT_SLINEAR11_FLOAT_IF(reg, fval) \
    do { if ((fval) != 0.0f) { \
        uint16_t _c = tps546_float_2_slinear11(fval); \
        if (emit_word(out, max, &n, (reg), _c) < 0) return -1; \
    } } while (0)

#define EMIT_SLINEAR11_INT_IF(reg, ival) \
    do { if ((ival) != 0) { \
        uint16_t _c = tps546_int_2_slinear11(ival); \
        if (emit_word(out, max, &n, (reg), _c) < 0) return -1; \
    } } while (0)

#define EMIT_ULINEAR16_IF(reg, fval) \
    do { if ((fval) != 0.0f) { \
        uint16_t _c = tps546_float_2_ulinear16((fval), vout_exp); \
        if (emit_word(out, max, &n, (reg), _c) < 0) return -1; \
    } } while (0)

// ---------------------------------------------------------------------------
// Public builder
// ---------------------------------------------------------------------------

int bb_power_tps546_build_init_program(
        const bb_power_tps546_cfg_t *cfg,
        int8_t vout_exp,
        bb_tps546_write_t *out,
        int max)
{
    if (!cfg || !out || max <= 0) return -1;

    const bb_power_tps546_protect_t *p = &cfg->protect;
    int n = 0;

    // --- ON_OFF_CONFIG (byte) ---
    EMIT_BYTE_IF(BB_PMBUS_ON_OFF_CONFIG, p->on_off_config);

    // --- STACK_CONFIG (word) ---
    EMIT_WORD_IF(BB_PMBUS_STACK_CONFIG, p->stack_config);

    // --- SYNC_CONFIG (byte) ---
    EMIT_BYTE_IF(BB_PMBUS_SYNC_CONFIG, p->sync_config);

    // --- PHASE (byte) ---
    EMIT_BYTE_IF(BB_PMBUS_PHASE, p->phase);

    // --- FREQUENCY_SWITCH (word, SLINEAR11 of int kHz) — essential ---
    {
        uint32_t freq = cfg->switch_freq_khz ? cfg->switch_freq_khz : 650u;
        uint16_t code = tps546_int_2_slinear11((int)freq);
        if (emit_word_ex(out, max, &n, BB_PMBUS_FREQUENCY_SWITCH, code, true) < 0) return -1;
    }

    // --- COMPENSATION_CONFIG (5-byte block, if any byte non-zero) ---
    {
        int any = 0;
        for (int i = 0; i < 5; i++) {
            if (p->compensation_config[i]) { any = 1; break; }
        }
        if (any) {
            if (emit_block5(out, max, &n, BB_PMBUS_COMPENSATION_CONFIG,
                            p->compensation_config) < 0) return -1;
        }
    }

    // --- VIN_UV_WARN_LIMIT (word, SLINEAR11 float V) — skip if 0 (AxeOS bug note) ---
    EMIT_SLINEAR11_FLOAT_IF(BB_PMBUS_VIN_UV_WARN_LIMIT, p->vin_uv_warn_v);

    // --- VIN_ON (word, SLINEAR11 float V) ---
    EMIT_SLINEAR11_FLOAT_IF(BB_PMBUS_VIN_ON, p->vin_on_v);

    // --- VIN_OFF (word, SLINEAR11 float V) ---
    EMIT_SLINEAR11_FLOAT_IF(BB_PMBUS_VIN_OFF, p->vin_off_v);

    // --- VIN_OV_FAULT_LIMIT (word, SLINEAR11 float V) ---
    EMIT_SLINEAR11_FLOAT_IF(BB_PMBUS_VIN_OV_FAULT_LIMIT, p->vin_ov_fault_v);

    // --- VIN_OV_FAULT_RESPONSE (byte) ---
    EMIT_BYTE_IF(BB_PMBUS_VIN_OV_FAULT_RESPONSE, p->vin_ov_fault_response);

    // --- VOUT_SCALE_LOOP (word, SLINEAR11 float) ---
    EMIT_SLINEAR11_FLOAT_IF(BB_PMBUS_VOUT_SCALE_LOOP, p->vout_scale_loop);

    // --- VOUT_COMMAND (word, ULINEAR16) — always written from target_mv, essential ---
    {
        float target_v = (float)cfg->target_mv / 1000.0f;
        uint16_t code = tps546_float_2_ulinear16(target_v, vout_exp);
        if (emit_word_ex(out, max, &n, BB_PMBUS_VOUT_COMMAND, code, true) < 0) return -1;
    }

    // --- VOUT_MAX (word, ULINEAR16 absolute V) ---
    EMIT_ULINEAR16_IF(BB_PMBUS_VOUT_MAX, p->vout_max_v);

    // --- VOUT_MIN (word, ULINEAR16 absolute V) ---
    EMIT_ULINEAR16_IF(BB_PMBUS_VOUT_MIN, p->vout_min_v);

    // --- VOUT OV/UV limits (word, ULINEAR16 of factor × target_V) ---
    {
        float target_v = (float)cfg->target_mv / 1000.0f;

        if (p->vout_ov_fault_factor != 0.0f) {
            uint16_t code = tps546_float_2_ulinear16(p->vout_ov_fault_factor * target_v, vout_exp);
            if (emit_word(out, max, &n, BB_PMBUS_VOUT_OV_FAULT_LIMIT, code) < 0) return -1;
        }
        if (p->vout_ov_warn_factor != 0.0f) {
            uint16_t code = tps546_float_2_ulinear16(p->vout_ov_warn_factor * target_v, vout_exp);
            if (emit_word(out, max, &n, BB_PMBUS_VOUT_OV_WARN_LIMIT, code) < 0) return -1;
        }
        if (p->vout_margin_high != 0.0f) {
            uint16_t code = tps546_float_2_ulinear16(p->vout_margin_high * target_v, vout_exp);
            if (emit_word(out, max, &n, BB_PMBUS_VOUT_MARGIN_HIGH, code) < 0) return -1;
        }
        if (p->vout_margin_low != 0.0f) {
            uint16_t code = tps546_float_2_ulinear16(p->vout_margin_low * target_v, vout_exp);
            if (emit_word(out, max, &n, BB_PMBUS_VOUT_MARGIN_LOW, code) < 0) return -1;
        }
        if (p->vout_uv_warn_factor != 0.0f) {
            uint16_t code = tps546_float_2_ulinear16(p->vout_uv_warn_factor * target_v, vout_exp);
            if (emit_word(out, max, &n, BB_PMBUS_VOUT_UV_WARN_LIMIT, code) < 0) return -1;
        }
        if (p->vout_uv_fault_factor != 0.0f) {
            uint16_t code = tps546_float_2_ulinear16(p->vout_uv_fault_factor * target_v, vout_exp);
            if (emit_word(out, max, &n, BB_PMBUS_VOUT_UV_FAULT_LIMIT, code) < 0) return -1;
        }
    }

    // --- IOUT_OC_WARN_LIMIT (word, SLINEAR11 float A) ---
    EMIT_SLINEAR11_FLOAT_IF(BB_PMBUS_IOUT_OC_WARN_LIMIT, p->iout_oc_warn_a);

    // --- IOUT_OC_FAULT_LIMIT (word, SLINEAR11 float A) — essential ---
    {
        float oc_a = cfg->oc_limit_a ? (float)cfg->oc_limit_a : 30.0f;
        uint16_t code = tps546_float_2_slinear11(oc_a);
        if (emit_word_ex(out, max, &n, BB_PMBUS_IOUT_OC_FAULT_LIMIT, code, true) < 0) return -1;
    }

    // --- IOUT_OC_FAULT_RESPONSE (byte) — essential ---
    {
        uint8_t resp = cfg->oc_response ? cfg->oc_response : 0xC0u;
        if (emit_byte_ex(out, max, &n, BB_PMBUS_IOUT_OC_FAULT_RESPONSE, resp, true) < 0) return -1;
    }

    // --- OT_WARN_LIMIT (word, SLINEAR11 int °C) ---
    EMIT_SLINEAR11_INT_IF(BB_PMBUS_OT_WARN_LIMIT, p->ot_warn_c);

    // --- OT_FAULT_LIMIT (word, SLINEAR11 int °C) ---
    EMIT_SLINEAR11_INT_IF(BB_PMBUS_OT_FAULT_LIMIT, p->ot_fault_c);

    // --- OT_FAULT_RESPONSE (byte) ---
    EMIT_BYTE_IF(BB_PMBUS_OT_FAULT_RESPONSE, p->ot_fault_response);

    // --- TON_DELAY (word, SLINEAR11 int ms) --- skip if 0
    if (p->ton_delay_ms != 0) {
        EMIT_SLINEAR11_INT_IF(BB_PMBUS_TON_DELAY, p->ton_delay_ms);
    }

    // --- TON_RISE (word, SLINEAR11 int ms) ---
    EMIT_SLINEAR11_INT_IF(BB_PMBUS_TON_RISE, p->ton_rise_ms);

    // --- TON_MAX_FAULT_LIMIT (word, SLINEAR11 int ms) --- skip if 0
    if (p->ton_max_fault_ms != 0) {
        EMIT_SLINEAR11_INT_IF(BB_PMBUS_TON_MAX_FAULT_LIMIT, p->ton_max_fault_ms);
    }

    // --- TON_MAX_FAULT_RESPONSE (byte) ---
    EMIT_BYTE_IF(BB_PMBUS_TON_MAX_FAULT_RESPONSE, p->ton_max_fault_response);

    return n;
}

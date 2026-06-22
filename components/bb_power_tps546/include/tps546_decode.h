// tps546_decode.h — pure PMBus ULINEAR16 / SLINEAR11 decode + encode helpers.
// No ESP-IDF dependency; host-testable. Ported from TaipanMiner tps546_decode.h.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

static inline int tps546_ulinear16_to_mv(uint16_t raw, int8_t exp_n)
{
    if (exp_n >= 0) {
        return (int)((uint32_t)raw * (1u << exp_n) * 1000u);
    }
    int shift = -exp_n;
    return (int)(((uint32_t)raw * 1000u + (1u << (shift - 1))) >> shift);
}

static inline int tps546_slinear11_decode(uint16_t raw, int *exp, int *mantissa)
{
    if (raw & 0x8000) {
        *exp = -((int)(((~raw >> 11) & 0x1Fu)) + 1);
    } else {
        *exp = (int)((raw >> 11) & 0x1Fu);
    }

    if (raw & 0x0400) {
        *mantissa = -((int)(~raw & 0x03FFu) + 1);
    } else {
        *mantissa = (int)(raw & 0x03FFu);
    }
    return 0;
}

// SLINEAR11 → integer, multiplied by `scale`. mV and mA use scale=1000;
// °C uses scale=1. The mv/ma/c_int wrappers below are the only callers.
static inline int tps546_slinear11_scaled(uint16_t raw, int scale)
{
    int exp, mantissa;
    tps546_slinear11_decode(raw, &exp, &mantissa);

    if (exp >= 0) {
        return mantissa * (1 << exp) * scale;
    }
    int shift = -exp;
    int64_t num = (int64_t)mantissa * scale;
    return (int)((num + (num >= 0 ? (1 << (shift - 1)) : -(1 << (shift - 1)))) >> shift);
}

static inline int tps546_slinear11_to_mv(uint16_t raw)    { return tps546_slinear11_scaled(raw, 1000); }
static inline int tps546_slinear11_to_ma(uint16_t raw)    { return tps546_slinear11_scaled(raw, 1000); }
static inline int tps546_slinear11_to_c_int(uint16_t raw) { return tps546_slinear11_scaled(raw, 1); }

// ---------------------------------------------------------------------------
// Encode helpers — ported from AxeOS TPS546.c (float_2_slinear11,
// float_2_ulinear16, int_2_slinear11). Pure / no I2C / host-testable.
// ---------------------------------------------------------------------------

// Encode a positive float value as SLINEAR11 (used for VIN, IOUT, FREQ as
// float). Returns 0 for non-positive inputs (matches AxeOS behaviour).
// Algorithm: find the negative exponent N such that mantissa = floor(v*2^N)
// fits in [0,1023], then encode as {sign-extended N in bits[15:11], mantissa
// in bits[10:0]}.
static inline uint16_t tps546_float_2_slinear11(float value)
{
    if (value <= 0.0f) return 0;

    int mantissa = 0;
    int exponent = 0;
    int i;
    for (i = 0; i <= 15; i++) {
        mantissa = (int)(value * powf(2.0f, (float)i));
        if (mantissa >= 1024) {
            exponent = i - 1;
            mantissa = (int)(value * powf(2.0f, (float)exponent));
            break;
        }
    }
    if (i == 16) return 0; /* could not find solution */

    // Exponent is stored negative in twos-complement in bits [15:11].
    // ~exponent+1 = two's complement negation.
    return (uint16_t)((((~exponent + 1) << 11) & 0xF800u) | (uint16_t)mantissa);
}

// Encode a positive integer value as SLINEAR11 (used for OT °C, TON ms,
// and switching frequency in kHz). Positive exponent encoding.
// Returns 0 for negative inputs.
static inline uint16_t tps546_int_2_slinear11(int value)
{
    if (value < 0) return 0;
    if (value == 0) return 0;

    int mantissa = value;
    int exponent = 0;
    int i;
    for (i = 0; i <= 15; i++) {
        mantissa = value / (int)(powf(2.0f, (float)i) + 0.5f);
        if (mantissa < 1024) {
            exponent = i;
            break;
        }
    }
    if (i == 16) return 0;

    return (uint16_t)(((exponent << 11) & 0xF800u) | (uint16_t)mantissa);
}

// ---------------------------------------------------------------------------
// STATUS register fault-bit helpers — pure / host-testable.
// ---------------------------------------------------------------------------

// Named fault-bit positions in the decoded fault mask returned by
// tps546_decode_fault_bits().  Bits are assigned to a uint16_t bitmask;
// the underlying PMBus register bits are noted beside each macro.
#define TPS546_FAULT_IOUT_OC  (1u << 0)  // STATUS_IOUT  bit 4 (IOUT_OC_FAULT)
#define TPS546_FAULT_OT       (1u << 1)  // STATUS_TEMPERATURE bit 7 (OT_FAULT)
#define TPS546_FAULT_VIN_UV   (1u << 2)  // STATUS_INPUT bit 3 (VIN_UV_WARNING)
#define TPS546_FAULT_VIN_OV   (1u << 3)  // STATUS_INPUT bit 5 (VIN_OV_FAULT)
#define TPS546_FAULT_UNIT_OFF (1u << 4)  // STATUS_WORD  bit 6 (UNIT_IS_OFF)

// Decode the four raw STATUS register values into a named-bit fault mask.
//
// Call order matters when CLEAR_FAULTS is issued elsewhere: read STATUS_IOUT
// for OC classification BEFORE issuing CLEAR_FAULTS so the OC bit is captured
// before it is cleared.  See op_poll() in bb_power_tps546.c for the sequencing
// comment.
//
// Returns 0 when no named faults are set; OR of TPS546_FAULT_* otherwise.
static inline uint16_t tps546_decode_fault_bits(uint16_t st_word,
                                                 uint8_t  st_iout,
                                                 uint8_t  st_temp,
                                                 uint8_t  st_input)
{
    uint16_t bits = 0;
    if (st_iout  & (1u << 4)) bits |= TPS546_FAULT_IOUT_OC;   // STATUS_IOUT b4
    if (st_temp  & (1u << 7)) bits |= TPS546_FAULT_OT;         // STATUS_TEMP b7
    if (st_input & (1u << 3)) bits |= TPS546_FAULT_VIN_UV;     // STATUS_INPUT b3
    if (st_input & (1u << 5)) bits |= TPS546_FAULT_VIN_OV;     // STATUS_INPUT b5
    if (st_word  & (1u << 6)) bits |= TPS546_FAULT_UNIT_OFF;   // STATUS_WORD b6
    return bits;
}

// VIN sag comparator: returns true when vin_mv is a valid reading (≥0) and
// below threshold_mv.  A negative vin_mv indicates an unavailable reading and
// is never treated as a sag.
static inline bool tps546_vin_sag(int vin_mv, int threshold_mv)
{
    return vin_mv >= 0 && vin_mv < threshold_mv;
}

// Encode a float voltage (in Volts) as ULINEAR16 using the VOUT_MODE
// exponent already read from the chip. exp_n is the signed 5-bit exponent
// (typically -9 on TPS546D24A). Formula: raw = value / 2^exp_n.
static inline uint16_t tps546_float_2_ulinear16(float value, int8_t exp_n)
{
    if (exp_n >= 0) {
        return (uint16_t)(value / (float)(1u << (unsigned)exp_n));
    }
    // exp_n < 0: multiply by 2^|exp_n|
    float scale = powf(2.0f, (float)(-exp_n));
    return (uint16_t)(value * scale);
}

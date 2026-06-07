// tps546_decode.h — pure PMBus ULINEAR16 / SLINEAR11 decode + encode helpers.
// No ESP-IDF dependency; host-testable. Ported from TaipanMiner tps546_decode.h.
#pragma once
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

static inline int tps546_slinear11_to_mv(uint16_t raw)
{
    int exp, mantissa;
    tps546_slinear11_decode(raw, &exp, &mantissa);

    if (exp >= 0) {
        return mantissa * (1 << exp) * 1000;
    }
    int shift = -exp;
    int64_t num = (int64_t)mantissa * 1000;
    return (int)((num + (num >= 0 ? (1 << (shift - 1)) : -(1 << (shift - 1)))) >> shift);
}

static inline int tps546_slinear11_to_c_int(uint16_t raw)
{
    int exp, mantissa;
    tps546_slinear11_decode(raw, &exp, &mantissa);

    if (exp >= 0) {
        return mantissa * (1 << exp);
    }
    int shift = -exp;
    int64_t num = (int64_t)mantissa;
    return (int)((num + (num >= 0 ? (1 << (shift - 1)) : -(1 << (shift - 1)))) >> shift);
}

static inline int tps546_slinear11_to_ma(uint16_t raw)
{
    int exp, mantissa;
    tps546_slinear11_decode(raw, &exp, &mantissa);

    if (exp >= 0) {
        return mantissa * (1 << exp) * 1000;
    }
    int shift = -exp;
    int64_t num = (int64_t)mantissa * 1000;
    return (int)((num + (num >= 0 ? (1 << (shift - 1)) : -(1 << (shift - 1)))) >> shift);
}

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

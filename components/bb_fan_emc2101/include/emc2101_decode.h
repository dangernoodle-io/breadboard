// emc2101_decode.h — pure EMC2101 register decode helpers.
// No ESP-IDF dependency; host-testable. Ported from TaipanMiner emc2101.c.
#pragma once
#include <stdint.h>

// Decode the 11-bit signed external diode temperature register (MSB+LSB, 0.125°C).
// raw_msb: REG_EXTERNAL_MSB (0x01), raw_lsb: REG_EXTERNAL_LSB (0x10).
// Returns temperature in degrees Celsius.
static inline float emc2101_decode_ext_temp(uint8_t raw_msb, uint8_t raw_lsb)
{
    uint16_t raw = ((uint16_t)raw_msb << 8) | raw_lsb;
    raw >>= 5;  // 11-bit signed
    int16_t signed_val = (int16_t)raw;
    if (raw & 0x400) {
        signed_val = (int16_t)(raw | 0xF800u);
    }
    return (float)signed_val / 8.0f;
}

// Decode internal temperature register (signed 8-bit, 1°C resolution).
// raw: REG_INTERNAL_TEMP (0x00).
static inline float emc2101_decode_int_temp(uint8_t raw)
{
    return (float)(int8_t)raw;
}

// Decode tach registers to RPM. Returns -1 if stalled (0xFFFF).
// raw_lsb: REG_TACH_LSB (0x46), raw_msb: REG_TACH_MSB (0x47).
static inline int emc2101_decode_rpm(uint8_t raw_lsb, uint8_t raw_msb)
{
    uint16_t tach = (uint16_t)raw_lsb | ((uint16_t)raw_msb << 8);
    if (tach == 0xFFFFu) return -1;
    return 5400000 / tach;
}

// Map a duty percentage [0,100] to the 6-bit fan setting register value [0,63].
static inline uint8_t emc2101_pct_to_duty(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (uint8_t)((pct * 63 + 50) / 100);
}

// Tests for emc2101_decode.h — pure decode math, no ESP-IDF dependency.
#include "unity.h"
#include "emc2101_decode.h"
#include <math.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// emc2101_decode_ext_temp — 11-bit signed, 0.125°C resolution
// ---------------------------------------------------------------------------

void test_emc2101_ext_temp_positive(void)
{
    // 72.0°C: 72 / 0.125 = 576 → 0x0240, shifted left by 5 → raw word 0x4800
    // MSB = 0x48, LSB = 0x00
    float t = emc2101_decode_ext_temp(0x48, 0x00);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 72.0f, t);
}

void test_emc2101_ext_temp_zero(void)
{
    float t = emc2101_decode_ext_temp(0x00, 0x00);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, t);
}

void test_emc2101_ext_temp_fractional(void)
{
    // 0.125°C: raw 11-bit = 1; stored in bits 15..5 of word → 0x0020
    // MSB = 0x00, LSB = 0x20
    float t = emc2101_decode_ext_temp(0x00, 0x20);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.125f, t);
}

void test_emc2101_ext_temp_negative(void)
{
    // -0.125°C: 11-bit two's complement = 0x7FF
    // Stored in bits 15..5 → word = 0x7FF << 5 = 0xFFE0
    // MSB = 0xFF, LSB = 0xE0
    float t = emc2101_decode_ext_temp(0xFF, 0xE0);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -0.125f, t);
}

void test_emc2101_ext_temp_minus_one(void)
{
    // -1.0°C: -8 in 11-bit signed (8 * 0.125 = 1.0)
    // -8 in 11-bit two's complement = 0x7F8; shifted left 5 = 0xFF00
    // MSB = 0xFF, LSB = 0x00
    float t = emc2101_decode_ext_temp(0xFF, 0x00);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, t);
}

// ---------------------------------------------------------------------------
// emc2101_decode_int_temp — signed 8-bit, 1°C
// ---------------------------------------------------------------------------

void test_emc2101_int_temp_positive(void)
{
    float t = emc2101_decode_int_temp(45);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.0f, t);
}

void test_emc2101_int_temp_zero(void)
{
    float t = emc2101_decode_int_temp(0);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, t);
}

void test_emc2101_int_temp_negative(void)
{
    // -10°C: 0xF6 as uint8_t → signed -10
    float t = emc2101_decode_int_temp(0xF6u);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -10.0f, t);
}

// ---------------------------------------------------------------------------
// emc2101_decode_rpm
// ---------------------------------------------------------------------------

void test_emc2101_rpm_typical(void)
{
    // 5400000 / 1688 ≈ 3199
    uint16_t tach = 1688;
    int rpm = emc2101_decode_rpm(tach & 0xFF, tach >> 8);
    TEST_ASSERT_EQUAL_INT(5400000 / 1688, rpm);
}

void test_emc2101_rpm_stalled(void)
{
    int rpm = emc2101_decode_rpm(0xFF, 0xFF);
    TEST_ASSERT_EQUAL_INT(-1, rpm);
}

void test_emc2101_rpm_max_speed(void)
{
    // tach = 1 → 5400000 RPM (theoretical max)
    int rpm = emc2101_decode_rpm(1, 0);
    TEST_ASSERT_EQUAL_INT(5400000, rpm);
}

// ---------------------------------------------------------------------------
// emc2101_pct_to_duty — [0,100] → [0,63] rounding
// ---------------------------------------------------------------------------

void test_emc2101_pct_to_duty_zero(void)
{
    TEST_ASSERT_EQUAL_INT(0, emc2101_pct_to_duty(0));
}

void test_emc2101_pct_to_duty_100(void)
{
    TEST_ASSERT_EQUAL_INT(63, emc2101_pct_to_duty(100));
}

void test_emc2101_pct_to_duty_50(void)
{
    // 50 * 63 / 100 = 31.5 → rounds to 32 with +50 bias
    TEST_ASSERT_EQUAL_INT(32, emc2101_pct_to_duty(50));
}

void test_emc2101_pct_to_duty_clamp_below_zero(void)
{
    TEST_ASSERT_EQUAL_INT(0, emc2101_pct_to_duty(-10));
}

void test_emc2101_pct_to_duty_clamp_above_100(void)
{
    TEST_ASSERT_EQUAL_INT(63, emc2101_pct_to_duty(110));
}

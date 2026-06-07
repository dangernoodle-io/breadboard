// Tests for tps546_decode.h — ULINEAR16 and SLINEAR11 pure decode functions.
// No ESP-IDF dependency; fully host-testable.
#include "unity.h"
#include "tps546_decode.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// tps546_ulinear16_to_mv
// ---------------------------------------------------------------------------

// With exponent -9: raw * 1000 / 512 (since shift=9, 2^9=512)
// Example: raw=0x0200 (512) → 512*1000/512 = 1000 mV
void test_ulinear16_to_mv_negative_exp(void)
{
    // exponent = -9, raw = 512 → 512 * 1000 >> 9 = 1000
    int mv = tps546_ulinear16_to_mv(512, -9);
    TEST_ASSERT_EQUAL_INT(1000, mv);
}

// exponent = -9, raw = 614 → floor((614 * 1000 + 256) / 512) = floor(614256/512) = 1199
// Actually: (614000 + 256) >> 9 = 614256 / 512 = 1199 mV (rounds)
void test_ulinear16_to_mv_rounding(void)
{
    int mv = tps546_ulinear16_to_mv(614, -9);
    // 614 * 1000 = 614000; (614000 + 256) >> 9 = 614256 / 512 = 1199.718... = 1199
    TEST_ASSERT_EQUAL_INT(1199, mv);
}

// exponent = -9, raw = 0 → 0 mV
void test_ulinear16_to_mv_zero_raw(void)
{
    TEST_ASSERT_EQUAL_INT(0, tps546_ulinear16_to_mv(0, -9));
}

// exponent = 0, raw = 1000 → 1000 * 1 * 1000 = 1000000 (not a realistic case but tests the exp≥0 branch)
void test_ulinear16_to_mv_zero_exp(void)
{
    int mv = tps546_ulinear16_to_mv(1, 0);
    TEST_ASSERT_EQUAL_INT(1000, mv);
}

// exponent = -8, raw = 256 → (256*1000 + 128) >> 8 = 256128/256 = 1000
void test_ulinear16_to_mv_exp_minus8(void)
{
    int mv = tps546_ulinear16_to_mv(256, -8);
    TEST_ASSERT_EQUAL_INT(1000, mv);
}

// ---------------------------------------------------------------------------
// tps546_slinear11_decode
// ---------------------------------------------------------------------------

// raw = 0x0000: exp=0, mantissa=0
void test_slinear11_decode_zero(void)
{
    int exp = 99, mantissa = 99;
    tps546_slinear11_decode(0x0000, &exp, &mantissa);
    TEST_ASSERT_EQUAL_INT(0, exp);
    TEST_ASSERT_EQUAL_INT(0, mantissa);
}

// raw = 0x0064 (100): exp=0, mantissa=100
void test_slinear11_decode_positive_mantissa(void)
{
    int exp, mantissa;
    tps546_slinear11_decode(0x0064, &exp, &mantissa);
    TEST_ASSERT_EQUAL_INT(0, exp);
    TEST_ASSERT_EQUAL_INT(100, mantissa);
}

// raw with negative mantissa: 0x0400 → bit 10 set
// mantissa = -((~0x0400 & 0x03FF) + 1) = -((0x03FF) + 1) = -(1024) = -1024...
// Wait: ~0x0400 = 0xFBFF; 0xFBFF & 0x03FF = 0x03FF = 1023; -1023-1 = no.
// Actually: raw=0x0400: raw&0x0400=non-zero, so mantissa = -((~raw & 0x03FF)+1)
// ~0x0400 (uint16) = 0xFBFF; 0xFBFF & 0x03FF = 0x03FF = 1023; -(1023+1) = -1024
// Hmm that's the minimum 11-bit twos complement value.
// Let's use raw=0x0401: bits[4:0] of upper = 0, bit10=1, lower 10 = 0x001
// ~0x0401 = 0xFBFE; 0xFBFE & 0x03FF = 0x03FE = 1022; -(1022+1) = -1023? No.
// Actually simpler: use raw=0x07FF (mantissa=-1, exp=0):
// raw&0x0400 != 0 so: mantissa = -((~0x07FF & 0x03FF)+1) = -((0xF800 & 0x03FF)+1)
// Hmm. ~(uint16_t)0x07FF = 0xF800; 0xF800 & 0x03FF = 0. mantissa = -(0+1) = -1.
void test_slinear11_decode_negative_mantissa_minus1(void)
{
    // raw=0x07FF: top 5 bits = 0 (exp=0), lower 11 bits = 0x7FF (bit10=1)
    // mantissa = -((~0x07FF & 0x03FF)+1) = -((0x0000)+1) = -1
    int exp, mantissa;
    tps546_slinear11_decode(0x07FF, &exp, &mantissa);
    TEST_ASSERT_EQUAL_INT(0, exp);
    TEST_ASSERT_EQUAL_INT(-1, mantissa);
}

// raw=0xB800: upper 5 bits = 0x17 (23), but bit15 set → negative exponent
// exp = -((~0xB800 >> 11) & 0x1F + 1)
// ~0xB800 = 0x47FF; >>11 = 0x08; &0x1F = 8; exp = -(8+1) = -9 ✓ (typical VOUT_MODE)
// lower 11 bits of 0xB800: 0x0000 → mantissa=0
void test_slinear11_decode_negative_exponent(void)
{
    int exp, mantissa;
    tps546_slinear11_decode(0xB800, &exp, &mantissa);
    TEST_ASSERT_EQUAL_INT(-9, exp);
    TEST_ASSERT_EQUAL_INT(0, mantissa);
}

// ---------------------------------------------------------------------------
// tps546_slinear11_to_ma
// ---------------------------------------------------------------------------

// Switching frequency 650 encoded as slinear11 with exp=0: raw=0x028A (650)
// to_ma: exp=0, mantissa=650 → 650 * 1 * 1000 = 650000 mA?
// (That's weird but that's what the function does: it treats the raw PMBus
// FREQUENCY_SWITCH value with the mA decode — in practice callers use the right
// decode for the right register. We test the math is correct for a 30A OC limit.)
// OC limit 30 A encoded as slinear11 exp=0, mantissa=30: raw=0x001E
// to_ma: 30 * 1 * 1000 = 30000 mA = 30 A ✓
void test_slinear11_to_ma_30a_oc(void)
{
    uint16_t raw = 30; // slinear11: exp=0, mantissa=30
    int ma = tps546_slinear11_to_ma(raw);
    TEST_ASSERT_EQUAL_INT(30000, ma);
}

// Current reading 10A: exp=0, mantissa=10 (raw=0x000A)
void test_slinear11_to_ma_10a(void)
{
    int ma = tps546_slinear11_to_ma(10);
    TEST_ASSERT_EQUAL_INT(10000, ma);
}

// Negative mantissa round-trip: -1 mA encoded as raw=0x07FF
void test_slinear11_to_ma_negative(void)
{
    // exp=0, mantissa=-1 → -1 * 1000 = -1000 mA
    int ma = tps546_slinear11_to_ma(0x07FF);
    TEST_ASSERT_EQUAL_INT(-1000, ma);
}

// ---------------------------------------------------------------------------
// tps546_slinear11_to_c_int
// ---------------------------------------------------------------------------

// Temperature 45°C: exp=0, mantissa=45 (raw=0x002D)
void test_slinear11_to_c_int_45c(void)
{
    int c = tps546_slinear11_to_c_int(45);
    TEST_ASSERT_EQUAL_INT(45, c);
}

// Temperature 0°C: raw=0
void test_slinear11_to_c_int_zero(void)
{
    TEST_ASSERT_EQUAL_INT(0, tps546_slinear11_to_c_int(0));
}

// ---------------------------------------------------------------------------
// tps546_slinear11_to_mv
// ---------------------------------------------------------------------------

// Input voltage 12V: 12000 mV. If exp=0, mantissa=12 → 12*1*1000=12000 mV
void test_slinear11_to_mv_12v(void)
{
    int mv = tps546_slinear11_to_mv(12);
    TEST_ASSERT_EQUAL_INT(12000, mv);
}

// 0V: raw=0
void test_slinear11_to_mv_zero(void)
{
    TEST_ASSERT_EQUAL_INT(0, tps546_slinear11_to_mv(0));
}

void test_slinear11_to_mv_5v(void)
{
    int mv = tps546_slinear11_to_mv(5);
    TEST_ASSERT_EQUAL_INT(5000, mv);
}

// Negative-exponent path: raw=0xF802 → exp=-1, mantissa=2 → 1000 mV (1.0V)
// Construction: bit15=1(neg), bits[14:11]=1111(~=0000→exp=-1), bit10=0, bits[9:0]=2
void test_slinear11_to_mv_negative_exp_path(void)
{
    // exp=-1, mantissa=2: value=2/2=1.0V=1000mV
    int mv = tps546_slinear11_to_mv(0xF802);
    TEST_ASSERT_EQUAL_INT(1000, mv);
}

// Negative-exponent + negative mantissa: raw=0xFFFE → exp=-1, mantissa=-2
// num=-2000, shift=1: (-2000 + (-1)) >> 1 = -2001 >> 1 = -1001 (arithmetic shift)
void test_slinear11_to_mv_neg_exp_neg_mantissa(void)
{
    int mv = tps546_slinear11_to_mv(0xFFFE);
    TEST_ASSERT_EQUAL_INT(-1001, mv);
}

// ---------------------------------------------------------------------------
// slinear11_to_c_int — negative-exp paths
// ---------------------------------------------------------------------------

// exp=-1, mantissa=2: value=2/2=1°C
void test_slinear11_to_c_int_negative_exp(void)
{
    int c = tps546_slinear11_to_c_int(0xF802);
    TEST_ASSERT_EQUAL_INT(1, c);
}

// exp=-1, mantissa=-2: num=-2, shift=1: (-2+(-1))>>1 = -3>>1 = -2 (arithmetic shift)
void test_slinear11_to_c_int_neg_exp_neg_mantissa(void)
{
    int c = tps546_slinear11_to_c_int(0xFFFE);
    TEST_ASSERT_EQUAL_INT(-2, c);
}

// ---------------------------------------------------------------------------
// slinear11_to_ma — negative-exp paths
// ---------------------------------------------------------------------------

// exp=-1, mantissa=2: value=2/2=1A=1000mA
void test_slinear11_to_ma_negative_exp(void)
{
    int ma = tps546_slinear11_to_ma(0xF802);
    TEST_ASSERT_EQUAL_INT(1000, ma);
}

// exp=-1, mantissa=-2: num=-2000, shift=1: (-2000+(-1))>>1 = -2001>>1 = -1001
void test_slinear11_to_ma_neg_exp_neg_mantissa(void)
{
    int ma = tps546_slinear11_to_ma(0xFFFE);
    TEST_ASSERT_EQUAL_INT(-1001, ma);
}

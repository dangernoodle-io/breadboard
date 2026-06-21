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

// ---------------------------------------------------------------------------
// tps546_decode_fault_bits
// ---------------------------------------------------------------------------

// Clean STATUS: STATUS_WORD=0x0800 (MFR_SPECIFIC bit only; no named-fault bits
// from STATUS_WORD bit6), all sub-registers zero → no named fault bits.
// The observed STATUS 0x0840 in the plan includes bit6 (UNIT_IS_OFF); using
// 0x0800 isolates the "truly clean" vector where no named bit fires.
void test_decode_fault_bits_clean(void)
{
    uint16_t bits = tps546_decode_fault_bits(0x0800, 0x00, 0x00, 0x00);
    TEST_ASSERT_EQUAL_UINT16(0, bits);
}

// STATUS_WORD=0x0840 has bit6 (UNIT_IS_OFF) set → TPS546_FAULT_UNIT_OFF fires.
// This is the actual STATUS seen during a VIN-UV-WARN-only event where the
// unit happens to have reported itself as off.
void test_decode_fault_bits_0x0840_unit_off(void)
{
    uint16_t bits = tps546_decode_fault_bits(0x0840, 0x00, 0x00, 0x00);
    TEST_ASSERT_TRUE(bits & TPS546_FAULT_UNIT_OFF);
}

// Fault scenario from the .80/650 board failure:
// STATUS_WORD=0x4851 has bit6 (UNIT_IS_OFF=0x40) set as well as OC bits.
// STATUS_IOUT=0x10: bit4=IOUT_OC_FAULT.
// Use 0x4851 to match the real-board STATUS_WORD; UNIT_OFF will also fire.
void test_decode_fault_bits_iout_oc(void)
{
    // st_iout = 0x10: bit4 set → IOUT_OC_FAULT
    // st_word = 0x4851: bit6 set → UNIT_OFF also fires
    uint16_t bits = tps546_decode_fault_bits(0x4851, 0x10, 0x00, 0x00);
    TEST_ASSERT_TRUE(bits & TPS546_FAULT_IOUT_OC);
    TEST_ASSERT_TRUE(bits & TPS546_FAULT_UNIT_OFF);  // bit6 in 0x4851
    // no OT, VIN_UV, or VIN_OV from these register values
    TEST_ASSERT_FALSE(bits & TPS546_FAULT_OT);
    TEST_ASSERT_FALSE(bits & TPS546_FAULT_VIN_UV);
    TEST_ASSERT_FALSE(bits & TPS546_FAULT_VIN_OV);
}

// Isolated IOUT_OC with no other named bits: use st_word=0x0010 (no bit6).
void test_decode_fault_bits_iout_oc_only(void)
{
    uint16_t bits = tps546_decode_fault_bits(0x0000, 0x10, 0x00, 0x00);
    TEST_ASSERT_EQUAL_UINT16(TPS546_FAULT_IOUT_OC, bits);
}

// OT fault: STATUS_TEMPERATURE bit7 set
void test_decode_fault_bits_ot(void)
{
    uint16_t bits = tps546_decode_fault_bits(0x0000, 0x00, 0x80, 0x00);
    TEST_ASSERT_TRUE(bits & TPS546_FAULT_OT);
    TEST_ASSERT_EQUAL_UINT16(TPS546_FAULT_OT, bits);
}

// VIN_UV: STATUS_INPUT bit3 set
void test_decode_fault_bits_vin_uv(void)
{
    uint16_t bits = tps546_decode_fault_bits(0x0000, 0x00, 0x00, 0x08);
    TEST_ASSERT_TRUE(bits & TPS546_FAULT_VIN_UV);
    TEST_ASSERT_EQUAL_UINT16(TPS546_FAULT_VIN_UV, bits);
}

// VIN_OV: STATUS_INPUT bit5 set
void test_decode_fault_bits_vin_ov(void)
{
    uint16_t bits = tps546_decode_fault_bits(0x0000, 0x00, 0x00, 0x20);
    TEST_ASSERT_TRUE(bits & TPS546_FAULT_VIN_OV);
    TEST_ASSERT_EQUAL_UINT16(TPS546_FAULT_VIN_OV, bits);
}

// UNIT_OFF: STATUS_WORD bit6 set
void test_decode_fault_bits_unit_off(void)
{
    uint16_t bits = tps546_decode_fault_bits(0x0040, 0x00, 0x00, 0x00);
    TEST_ASSERT_TRUE(bits & TPS546_FAULT_UNIT_OFF);
    TEST_ASSERT_EQUAL_UINT16(TPS546_FAULT_UNIT_OFF, bits);
}

// Multiple faults OR'd together: IOUT_OC + OT + VIN_UV
void test_decode_fault_bits_multiple(void)
{
    // st_iout bit4, st_temp bit7, st_input bit3
    uint16_t bits = tps546_decode_fault_bits(0x0000, 0x10, 0x80, 0x08);
    TEST_ASSERT_TRUE(bits & TPS546_FAULT_IOUT_OC);
    TEST_ASSERT_TRUE(bits & TPS546_FAULT_OT);
    TEST_ASSERT_TRUE(bits & TPS546_FAULT_VIN_UV);
    TEST_ASSERT_FALSE(bits & TPS546_FAULT_VIN_OV);
    TEST_ASSERT_FALSE(bits & TPS546_FAULT_UNIT_OFF);
}

// ---------------------------------------------------------------------------
// tps546_vin_sag
// ---------------------------------------------------------------------------

// Below threshold: sag
void test_vin_sag_below_threshold(void)
{
    TEST_ASSERT_TRUE(tps546_vin_sag(4500, 4600));
}

// At threshold: not a sag (strict less-than)
void test_vin_sag_at_threshold(void)
{
    TEST_ASSERT_FALSE(tps546_vin_sag(4600, 4600));
}

// Above threshold: not a sag
void test_vin_sag_above_threshold(void)
{
    TEST_ASSERT_FALSE(tps546_vin_sag(5000, 4600));
}

// Unavailable reading (vin_mv < 0): never a sag
void test_vin_sag_unavailable(void)
{
    TEST_ASSERT_FALSE(tps546_vin_sag(-1, 4600));
}

// Zero vin with positive threshold: treated as sag (0 is a valid reading below threshold)
void test_vin_sag_zero_vin(void)
{
    TEST_ASSERT_TRUE(tps546_vin_sag(0, 4600));
}

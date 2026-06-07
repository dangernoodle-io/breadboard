// Tests for bb_power_tps546_build_init_program and encode helpers.
// No ESP-IDF dependency; fully host-testable.
#include "unity.h"
#include "tps546_decode.h"
#include "bb_power_tps546.h"
#include <stdint.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Encoder unit tests
// ---------------------------------------------------------------------------

// float_2_slinear11: 6.5 V (VIN_OV for default family)
// Algorithm: find i where floor(6.5 * 2^i) >= 1024:
//   i=0: 6, i=1: 13, i=2: 26, i=3: 52, i=4: 104, i=5: 208, i=6: 416, i=7: 832,
//   i=8: 1664 >= 1024 → exponent=7, mantissa=floor(6.5*128)=832
// Encoded: (~7+1)=0xF9 → (0xF9 << 11) & 0xF800 = 0xC800; result = 0xC800 | 832
// 832 = 0x0340 → result = 0xCB40
void test_encode_float_slinear11_6v5(void)
{
    uint16_t code = tps546_float_2_slinear11(6.5f);
    // Decode back and check within 1 LSB
    int exp, mantissa;
    tps546_slinear11_decode(code, &exp, &mantissa);
    // exp should be negative (stored as 2s complement in upper 5 bits)
    float result = (float)mantissa * powf(2.0f, (float)exp);
    // Allow 1% tolerance due to float truncation
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 6.5f, result);
}

// float_2_slinear11: 4.8 V (VIN_ON for default family)
void test_encode_float_slinear11_4v8(void)
{
    uint16_t code = tps546_float_2_slinear11(4.8f);
    int exp, mantissa;
    tps546_slinear11_decode(code, &exp, &mantissa);
    float result = (float)mantissa * powf(2.0f, (float)exp);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 4.8f, result);
}

// float_2_slinear11: 4.5 V (VIN_OFF for default family)
void test_encode_float_slinear11_4v5(void)
{
    uint16_t code = tps546_float_2_slinear11(4.5f);
    int exp, mantissa;
    tps546_slinear11_decode(code, &exp, &mantissa);
    float result = (float)mantissa * powf(2.0f, (float)exp);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 4.5f, result);
}

// float_2_slinear11: 0.25 (VOUT_SCALE_LOOP)
void test_encode_float_slinear11_0v25(void)
{
    uint16_t code = tps546_float_2_slinear11(0.25f);
    int exp, mantissa;
    tps546_slinear11_decode(code, &exp, &mantissa);
    float result = (float)mantissa * powf(2.0f, (float)exp);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.25f, result);
}

// float_2_slinear11: zero returns 0
void test_encode_float_slinear11_zero(void)
{
    TEST_ASSERT_EQUAL_UINT16(0, tps546_float_2_slinear11(0.0f));
}

// float_2_slinear11: negative returns 0
void test_encode_float_slinear11_negative(void)
{
    TEST_ASSERT_EQUAL_UINT16(0, tps546_float_2_slinear11(-1.0f));
}

// int_2_slinear11: 650 kHz switching frequency.
// 650 < 1024 → exponent=0, mantissa=650 → code=0x028A
void test_encode_int_slinear11_650(void)
{
    uint16_t code = tps546_int_2_slinear11(650);
    // exponent=0: upper 5 bits = 0, lower 11 = 650
    TEST_ASSERT_EQUAL_UINT16(650u, code & 0x07FFu);   // mantissa=650
    TEST_ASSERT_EQUAL_UINT16(0u, (code >> 11) & 0x1Fu); // exponent=0
}

// int_2_slinear11 round-trip: 105 °C (OT_WARN)
void test_encode_int_slinear11_105c(void)
{
    uint16_t code = tps546_int_2_slinear11(105);
    int result = tps546_slinear11_to_c_int(code);
    TEST_ASSERT_INT_WITHIN(1, 105, result);
}

// int_2_slinear11 round-trip: 145 °C (OT_FAULT)
void test_encode_int_slinear11_145c(void)
{
    uint16_t code = tps546_int_2_slinear11(145);
    int result = tps546_slinear11_to_c_int(code);
    TEST_ASSERT_INT_WITHIN(1, 145, result);
}

// int_2_slinear11: 3 ms (TON_RISE)
void test_encode_int_slinear11_3ms(void)
{
    uint16_t code = tps546_int_2_slinear11(3);
    // 3 < 1024 → exp=0, mantissa=3
    TEST_ASSERT_EQUAL_UINT16(3u, code);
}

// int_2_slinear11: 0 returns 0
void test_encode_int_slinear11_zero(void)
{
    TEST_ASSERT_EQUAL_UINT16(0, tps546_int_2_slinear11(0));
}

// int_2_slinear11: negative value returns 0 (line 108 branch)
void test_encode_int_slinear11_negative(void)
{
    TEST_ASSERT_EQUAL_UINT16(0, tps546_int_2_slinear11(-1));
}

// int_2_slinear11: huge value (>= 1024*2^15 = 33554432) exhausts the 0..15
// exponent loop and returns 0 — covers the "i==16" exit (lines 114/116/121).
void test_encode_int_slinear11_loop_exhausted(void)
{
    // 34000000 / 2^15 = ~1037 >= 1024, so loop never breaks → returns 0
    TEST_ASSERT_EQUAL_UINT16(0, tps546_int_2_slinear11(34000000));
}

// float_2_slinear11: value >= 1024 hits mantissa >= 1024 at i=0, uses exponent=-1.
// At i=0: mantissa=floor(1500*1)=1500 >= 1024 → exponent=0-1=-1,
// mantissa=floor(1500*0.5)=750. Encode: (~(-1)+1)=0 → stored neg=0 in upper
// bits? Wait: ~(-1)=0, 0+1=1 → (1<<11)&0xF800=0x0800. mantissa=750.
// Round-trip: decode exponent from 0x0800>>11=1 → but stored sign ext...
// Just verify the round-trip within tolerance.
void test_encode_float_slinear11_large_value(void)
{
    // 1500.0f: mantissa>=1024 at i=0, forcing the exponent=i-1=-1 branch.
    uint16_t code = tps546_float_2_slinear11(1500.0f);
    int exp, mantissa;
    tps546_slinear11_decode(code, &exp, &mantissa);
    float result = (float)mantissa * powf(2.0f, (float)exp);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 1500.0f, result);
}

// float_2_slinear11: tiny positive value < 0.03125 exhausts the 0..15 exponent
// loop (even at i=15, floor(v*32768) < 1024) and returns 0 — covers line 96.
void test_encode_float_slinear11_loop_exhausted(void)
{
    // 0.001f: floor(0.001 * 32768) = 32, never reaches 1024 → returns 0
    TEST_ASSERT_EQUAL_UINT16(0, tps546_float_2_slinear11(0.001f));
}

// float_2_ulinear16: positive exponent branch (exp_n >= 0, line 131).
// value=4.0f, exp_n=2: raw = 4.0 / 2^2 = 4.0/4 = 1 → code=1
void test_encode_ulinear16_positive_exp(void)
{
    uint16_t code = tps546_float_2_ulinear16(4.0f, 2);
    TEST_ASSERT_EQUAL_UINT16(1u, code);
}

// float_2_ulinear16: positive exponent, exp_n=0: raw = value / 1 = value
void test_encode_ulinear16_zero_exp(void)
{
    uint16_t code = tps546_float_2_ulinear16(10.0f, 0);
    TEST_ASSERT_EQUAL_UINT16(10u, code);
}

// float_2_ulinear16: 2.0 V with exp=-9
// 2.0 * 2^9 = 2.0 * 512 = 1024 → code=1024
void test_encode_ulinear16_2v_exp_minus9(void)
{
    uint16_t code = tps546_float_2_ulinear16(2.0f, -9);
    TEST_ASSERT_EQUAL_UINT16(1024u, code);
}

// float_2_ulinear16: 1.0 V with exp=-9 → 512
void test_encode_ulinear16_1v_exp_minus9(void)
{
    uint16_t code = tps546_float_2_ulinear16(1.0f, -9);
    TEST_ASSERT_EQUAL_UINT16(512u, code);
}

// round-trip: encode then decode within 1 mV for 1.15 V
void test_encode_ulinear16_roundtrip_1v15(void)
{
    int8_t exp = -9;
    uint16_t code = tps546_float_2_ulinear16(1.15f, exp);
    int decoded_mv = tps546_ulinear16_to_mv(code, exp);
    TEST_ASSERT_INT_WITHIN(2, 1150, decoded_mv);
}

// round-trip: encode then decode 1.4375 V (OV_FAULT for 1.15 V target, factor 1.25)
void test_encode_ulinear16_roundtrip_ov_fault(void)
{
    int8_t exp = -9;
    float threshold = 1.25f * 1.15f; // = 1.4375 V
    uint16_t code = tps546_float_2_ulinear16(threshold, exp);
    int decoded_mv = tps546_ulinear16_to_mv(code, exp);
    TEST_ASSERT_INT_WITHIN(3, (int)(threshold * 1000.0f), decoded_mv);
}

// ---------------------------------------------------------------------------
// build_init_program: default-family config (target 1150 mV)
// ---------------------------------------------------------------------------

// Build a full default-family protect cfg matching AxeOS default case
static bb_power_tps546_cfg_t make_default_family_cfg(void)
{
    bb_power_tps546_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.target_mv       = 1150;
    cfg.switch_freq_khz = 650;
    cfg.oc_limit_a      = 30;
    cfg.oc_response     = 0xC0;

    bb_power_tps546_protect_t *p = &cfg.protect;
    p->on_off_config        = 0x1Au; // ON_OFF_CONFIG_DELAY|CMD|PU (matches AxeOS)
    p->stack_config         = 0x0000u;
    p->sync_config          = 0x10u;
    p->phase                = 0x00u; // single-phase
    p->vin_on_v             = 4.8f;
    p->vin_off_v            = 4.5f;
    p->vin_uv_warn_v        = 0.0f; // skip (default family)
    p->vin_ov_fault_v       = 6.5f;
    p->vin_ov_fault_response = 0xB7u;
    p->vout_scale_loop      = 0.25f;
    p->vout_max_v           = 2.0f;
    p->vout_min_v           = 1.0f;
    p->vout_ov_fault_factor = 1.25f;
    p->vout_ov_warn_factor  = 1.16f;
    p->vout_margin_high     = 1.10f;
    p->vout_margin_low      = 0.90f;
    p->vout_uv_warn_factor  = 0.90f;
    p->vout_uv_fault_factor = 0.75f;
    p->iout_oc_warn_a       = 25.0f;
    p->ot_warn_c            = 105;
    p->ot_fault_c           = 145;
    p->ot_fault_response    = 0xFFu;
    p->ton_delay_ms         = 0; // skip (AxeOS default is 0)
    p->ton_rise_ms          = 3;
    p->ton_max_fault_ms     = 0; // skip (AxeOS default is 0)
    p->ton_max_fault_response = 0; // skip
    // compensation_config all zero → skipped
    return cfg;
}

// Helper: find a write entry by register address
static const bb_tps546_write_t *find_write(const bb_tps546_write_t *prog, int n, uint8_t reg)
{
    for (int i = 0; i < n; i++) {
        if (prog[i].reg == reg) return &prog[i];
    }
    return NULL;
}

// Helper: get index of a write entry
static int find_write_idx(const bb_tps546_write_t *prog, int n, uint8_t reg)
{
    for (int i = 0; i < n; i++) {
        if (prog[i].reg == reg) return i;
    }
    return -1;
}

#define EXP_N ((int8_t)-9)

void test_program_returns_positive_count(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
}

void test_program_null_cfg_returns_neg1(void)
{
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(NULL, EXP_N, prog, 40);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_program_null_out_returns_neg1(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, NULL, 40);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

void test_program_zero_max_returns_neg1(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[1];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 0);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

// (a) Encoder correctness: ON_OFF_CONFIG byte
void test_program_on_off_config_present(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_ON_OFF_CONFIG);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT8(BB_TPS546_W_BYTE, w->width);
    TEST_ASSERT_EQUAL_UINT8(0x1Au, (uint8_t)w->word);
}

// STACK_CONFIG word present and correct
void test_program_stack_config_present(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    // stack_config=0 is falsy; use a non-zero value
    cfg.protect.stack_config = 0x0001u;
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_STACK_CONFIG);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT8(BB_TPS546_W_WORD, w->width);
    TEST_ASSERT_EQUAL_UINT16(0x0001u, w->word);
}

// FREQUENCY_SWITCH always present; encoded as int SLINEAR11
void test_program_frequency_switch_present(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_FREQUENCY_SWITCH);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT8(BB_TPS546_W_WORD, w->width);
    // 650 kHz: exponent=0, mantissa=650 → word=650
    TEST_ASSERT_EQUAL_UINT16(650u, w->word);
}

// VIN_UV_WARN_LIMIT skipped when vin_uv_warn_v=0 (default family)
void test_program_vin_uv_warn_skipped_when_zero(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_VIN_UV_WARN_LIMIT));
}

// VIN_ON encoded as float SLINEAR11 and decodes to ~4.8 V
void test_program_vin_on_encoded_correctly(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_VIN_ON);
    TEST_ASSERT_NOT_NULL(w);
    int exp, mantissa;
    tps546_slinear11_decode(w->word, &exp, &mantissa);
    float v = (float)mantissa * powf(2.0f, (float)exp);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 4.8f, v);
}

// VIN_OV_FAULT_LIMIT encoded as float SLINEAR11 → ~6.5 V
void test_program_vin_ov_fault_encoded_correctly(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_VIN_OV_FAULT_LIMIT);
    TEST_ASSERT_NOT_NULL(w);
    int exp, mantissa;
    tps546_slinear11_decode(w->word, &exp, &mantissa);
    float v = (float)mantissa * powf(2.0f, (float)exp);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 6.5f, v);
}

// VIN_OV_FAULT_RESPONSE byte = 0xB7
void test_program_vin_ov_fault_response_correct(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_VIN_OV_FAULT_RESPONSE);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT8(0xB7u, (uint8_t)w->word);
}

// VOUT_COMMAND: encoded from target_mv=1150 → decodes to ~1150 mV
void test_program_vout_command_encoded_correctly(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_VOUT_COMMAND);
    TEST_ASSERT_NOT_NULL(w);
    int decoded_mv = tps546_ulinear16_to_mv(w->word, EXP_N);
    TEST_ASSERT_INT_WITHIN(2, 1150, decoded_mv);
}

// (b) VOUT_OV_FAULT_LIMIT = 1.25 × 1150 mV = 1437.5 mV ≈ 1438 mV
void test_program_vout_ov_fault_factor_correct(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_VOUT_OV_FAULT_LIMIT);
    TEST_ASSERT_NOT_NULL(w);
    int decoded_mv = tps546_ulinear16_to_mv(w->word, EXP_N);
    // 1.25 × 1150 = 1437.5; allow ±3 mV for ULINEAR16 quantisation
    TEST_ASSERT_INT_WITHIN(3, 1438, decoded_mv);
}

// VOUT_OV_WARN_LIMIT = 1.16 × 1150 = 1334 mV
void test_program_vout_ov_warn_factor_correct(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_VOUT_OV_WARN_LIMIT);
    TEST_ASSERT_NOT_NULL(w);
    int decoded_mv = tps546_ulinear16_to_mv(w->word, EXP_N);
    TEST_ASSERT_INT_WITHIN(3, 1334, decoded_mv);
}

// VOUT_UV_FAULT_LIMIT = 0.75 × 1150 = 862.5 mV ≈ 863 mV
void test_program_vout_uv_fault_factor_correct(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_VOUT_UV_FAULT_LIMIT);
    TEST_ASSERT_NOT_NULL(w);
    int decoded_mv = tps546_ulinear16_to_mv(w->word, EXP_N);
    TEST_ASSERT_INT_WITHIN(3, 863, decoded_mv);
}

// IOUT_OC_FAULT_LIMIT = 30 A (from cfg.oc_limit_a)
void test_program_iout_oc_fault_limit_correct(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_IOUT_OC_FAULT_LIMIT);
    TEST_ASSERT_NOT_NULL(w);
    int ma = tps546_slinear11_to_ma(w->word);
    TEST_ASSERT_INT_WITHIN(100, 30000, ma);
}

// IOUT_OC_FAULT_RESPONSE = 0xC0
void test_program_iout_oc_response_correct(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_IOUT_OC_FAULT_RESPONSE);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT8(0xC0u, (uint8_t)w->word);
}

// OT_WARN_LIMIT = 105 °C
void test_program_ot_warn_encoded_correctly(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_OT_WARN_LIMIT);
    TEST_ASSERT_NOT_NULL(w);
    int c = tps546_slinear11_to_c_int(w->word);
    TEST_ASSERT_INT_WITHIN(1, 105, c);
}

// OT_FAULT_LIMIT = 145 °C
void test_program_ot_fault_encoded_correctly(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_OT_FAULT_LIMIT);
    TEST_ASSERT_NOT_NULL(w);
    int c = tps546_slinear11_to_c_int(w->word);
    TEST_ASSERT_INT_WITHIN(1, 145, c);
}

// OT_FAULT_RESPONSE = 0xFF
void test_program_ot_fault_response_correct(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_OT_FAULT_RESPONSE);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT8(0xFFu, (uint8_t)w->word);
}

// TON_RISE = 3 ms
void test_program_ton_rise_encoded_correctly(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_TON_RISE);
    TEST_ASSERT_NOT_NULL(w);
    int ms = tps546_slinear11_to_c_int(w->word); // same decoding for integer ms
    TEST_ASSERT_EQUAL_INT(3, ms);
}

// (c) Zero fields are skipped
// With empty protect (all zero) only FREQUENCY_SWITCH + VOUT_COMMAND + OC_FAULT + OC_RESPONSE are written
void test_program_zero_protect_skips_all_protection_writes(void)
{
    bb_power_tps546_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.target_mv       = 1200;
    cfg.switch_freq_khz = 650;
    cfg.oc_limit_a      = 30;
    cfg.oc_response     = 0xC0;
    // protect all-zero

    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    // These must be present (always written)
    TEST_ASSERT_NOT_NULL(find_write(prog, n, BB_PMBUS_FREQUENCY_SWITCH));
    TEST_ASSERT_NOT_NULL(find_write(prog, n, BB_PMBUS_VOUT_COMMAND));
    TEST_ASSERT_NOT_NULL(find_write(prog, n, BB_PMBUS_IOUT_OC_FAULT_LIMIT));
    TEST_ASSERT_NOT_NULL(find_write(prog, n, BB_PMBUS_IOUT_OC_FAULT_RESPONSE));

    // These must be absent (all protect fields are zero)
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_ON_OFF_CONFIG));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_STACK_CONFIG));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_SYNC_CONFIG));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_PHASE));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_COMPENSATION_CONFIG));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_VIN_UV_WARN_LIMIT));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_VIN_ON));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_VIN_OFF));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_VIN_OV_FAULT_LIMIT));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_VIN_OV_FAULT_RESPONSE));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_VOUT_SCALE_LOOP));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_VOUT_MAX));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_VOUT_MIN));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_VOUT_OV_FAULT_LIMIT));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_VOUT_OV_WARN_LIMIT));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_VOUT_MARGIN_HIGH));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_VOUT_MARGIN_LOW));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_VOUT_UV_WARN_LIMIT));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_VOUT_UV_FAULT_LIMIT));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_IOUT_OC_WARN_LIMIT));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_OT_WARN_LIMIT));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_OT_FAULT_LIMIT));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_OT_FAULT_RESPONSE));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_TON_DELAY));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_TON_RISE));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_TON_MAX_FAULT_LIMIT));
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_TON_MAX_FAULT_RESPONSE));
}

// (d) Program order matches AxeOS write_entire_config() order.
// Use non-zero values for all fields so they all appear in the program.
// Order: ON_OFF_CONFIG < STACK_CONFIG < SYNC_CONFIG < PHASE < FREQUENCY_SWITCH
//        < VIN_ON < VIN_OFF < VIN_OV_FAULT_LIMIT < VIN_OV_FAULT_RESPONSE
//        < VOUT_SCALE_LOOP < VOUT_COMMAND < VOUT_MAX < VOUT_MIN
//        < VOUT_OV_FAULT_LIMIT < VOUT_OV_WARN_LIMIT < VOUT_MARGIN_HIGH
//        < VOUT_MARGIN_LOW < VOUT_UV_WARN_LIMIT < VOUT_UV_FAULT_LIMIT
//        < IOUT_OC_WARN_LIMIT < IOUT_OC_FAULT_LIMIT < IOUT_OC_FAULT_RESPONSE
//        < OT_WARN_LIMIT < OT_FAULT_LIMIT < OT_FAULT_RESPONSE < TON_RISE
void test_program_order_matches_axeos(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    cfg.protect.stack_config   = 0x0001u; // non-zero so it appears
    cfg.protect.phase          = 0x01u;   // non-zero so it appears (0x00=single is skip)
    cfg.protect.iout_oc_warn_a = 25.0f;

    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    int idx_on_off   = find_write_idx(prog, n, BB_PMBUS_ON_OFF_CONFIG);
    int idx_stack    = find_write_idx(prog, n, BB_PMBUS_STACK_CONFIG);
    int idx_sync     = find_write_idx(prog, n, BB_PMBUS_SYNC_CONFIG);
    int idx_phase    = find_write_idx(prog, n, BB_PMBUS_PHASE);
    int idx_freq     = find_write_idx(prog, n, BB_PMBUS_FREQUENCY_SWITCH);
    int idx_vin_on   = find_write_idx(prog, n, BB_PMBUS_VIN_ON);
    int idx_vin_off  = find_write_idx(prog, n, BB_PMBUS_VIN_OFF);
    int idx_vin_ovf  = find_write_idx(prog, n, BB_PMBUS_VIN_OV_FAULT_LIMIT);
    int idx_vin_ovr  = find_write_idx(prog, n, BB_PMBUS_VIN_OV_FAULT_RESPONSE);
    int idx_vscale   = find_write_idx(prog, n, BB_PMBUS_VOUT_SCALE_LOOP);
    int idx_vcmd     = find_write_idx(prog, n, BB_PMBUS_VOUT_COMMAND);
    int idx_vmax     = find_write_idx(prog, n, BB_PMBUS_VOUT_MAX);
    int idx_vmin     = find_write_idx(prog, n, BB_PMBUS_VOUT_MIN);
    int idx_vovf     = find_write_idx(prog, n, BB_PMBUS_VOUT_OV_FAULT_LIMIT);
    int idx_vovw     = find_write_idx(prog, n, BB_PMBUS_VOUT_OV_WARN_LIMIT);
    int idx_vmh      = find_write_idx(prog, n, BB_PMBUS_VOUT_MARGIN_HIGH);
    int idx_vml      = find_write_idx(prog, n, BB_PMBUS_VOUT_MARGIN_LOW);
    int idx_vuvw     = find_write_idx(prog, n, BB_PMBUS_VOUT_UV_WARN_LIMIT);
    int idx_vuvf     = find_write_idx(prog, n, BB_PMBUS_VOUT_UV_FAULT_LIMIT);
    int idx_ioc_warn = find_write_idx(prog, n, BB_PMBUS_IOUT_OC_WARN_LIMIT);
    int idx_ioc_flt  = find_write_idx(prog, n, BB_PMBUS_IOUT_OC_FAULT_LIMIT);
    int idx_ioc_rsp  = find_write_idx(prog, n, BB_PMBUS_IOUT_OC_FAULT_RESPONSE);
    int idx_ot_warn  = find_write_idx(prog, n, BB_PMBUS_OT_WARN_LIMIT);
    int idx_ot_flt   = find_write_idx(prog, n, BB_PMBUS_OT_FAULT_LIMIT);
    int idx_ot_rsp   = find_write_idx(prog, n, BB_PMBUS_OT_FAULT_RESPONSE);
    int idx_ton_rise = find_write_idx(prog, n, BB_PMBUS_TON_RISE);

    // All expected entries must be present
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, idx_on_off);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, idx_stack);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, idx_sync);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, idx_phase);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, idx_freq);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, idx_vin_on);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, idx_vcmd);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, idx_ton_rise);

    // AxeOS order: on_off < stack < sync < phase < freq ...
    // Unity TEST_ASSERT_LESS_THAN_INT(threshold, actual) checks actual < threshold.
    // To assert A comes before B (idx_A < idx_B): TEST_ASSERT_LESS_THAN_INT(idx_B, idx_A).
    TEST_ASSERT_LESS_THAN_INT(idx_stack,    idx_on_off);  // on_off < stack
    TEST_ASSERT_LESS_THAN_INT(idx_sync,     idx_stack);   // stack < sync
    TEST_ASSERT_LESS_THAN_INT(idx_phase,    idx_sync);    // sync < phase
    TEST_ASSERT_LESS_THAN_INT(idx_freq,     idx_phase);   // phase < freq
    TEST_ASSERT_LESS_THAN_INT(idx_vin_on,   idx_freq);    // freq < vin_on
    TEST_ASSERT_LESS_THAN_INT(idx_vin_off,  idx_vin_on);  // vin_on < vin_off
    TEST_ASSERT_LESS_THAN_INT(idx_vin_ovf,  idx_vin_off); // vin_off < vin_ovf
    TEST_ASSERT_LESS_THAN_INT(idx_vin_ovr,  idx_vin_ovf); // vin_ovf < vin_ovr
    TEST_ASSERT_LESS_THAN_INT(idx_vscale,   idx_vin_ovr); // vin_ovr < vscale
    TEST_ASSERT_LESS_THAN_INT(idx_vcmd,     idx_vscale);  // vscale < vcmd
    TEST_ASSERT_LESS_THAN_INT(idx_vmax,     idx_vcmd);    // vcmd < vmax
    TEST_ASSERT_LESS_THAN_INT(idx_vmin,     idx_vmax);    // vmax < vmin
    TEST_ASSERT_LESS_THAN_INT(idx_vovf,     idx_vmin);    // vmin < vovf
    TEST_ASSERT_LESS_THAN_INT(idx_vovw,     idx_vovf);    // vovf < vovw
    TEST_ASSERT_LESS_THAN_INT(idx_vmh,      idx_vovw);    // vovw < vmh
    TEST_ASSERT_LESS_THAN_INT(idx_vml,      idx_vmh);     // vmh < vml
    TEST_ASSERT_LESS_THAN_INT(idx_vuvw,     idx_vml);     // vml < vuvw
    TEST_ASSERT_LESS_THAN_INT(idx_vuvf,     idx_vuvw);    // vuvw < vuvf
    TEST_ASSERT_LESS_THAN_INT(idx_ioc_warn, idx_vuvf);    // vuvf < ioc_warn
    TEST_ASSERT_LESS_THAN_INT(idx_ioc_flt,  idx_ioc_warn);// ioc_warn < ioc_flt
    TEST_ASSERT_LESS_THAN_INT(idx_ioc_rsp,  idx_ioc_flt); // ioc_flt < ioc_rsp
    TEST_ASSERT_LESS_THAN_INT(idx_ot_warn,  idx_ioc_rsp); // ioc_rsp < ot_warn
    TEST_ASSERT_LESS_THAN_INT(idx_ot_flt,   idx_ot_warn); // ot_warn < ot_flt
    TEST_ASSERT_LESS_THAN_INT(idx_ot_rsp,   idx_ot_flt);  // ot_flt < ot_rsp
    TEST_ASSERT_LESS_THAN_INT(idx_ton_rise, idx_ot_rsp);  // ot_rsp < ton_rise
}

// Overflow: program buffer too small returns -1
void test_program_overflow_returns_neg1(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[2]; // way too small
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 2);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

// COMPENSATION_CONFIG block: only written when non-zero
void test_program_compensation_skipped_when_zero(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    // compensation_config all zero by default in make_default_family_cfg
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    TEST_ASSERT_NULL(find_write(prog, n, BB_PMBUS_COMPENSATION_CONFIG));
}

void test_program_compensation_written_when_nonzero(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    cfg.protect.compensation_config[0] = 0x12;
    cfg.protect.compensation_config[1] = 0x34;
    cfg.protect.compensation_config[2] = 0x42;
    cfg.protect.compensation_config[3] = 0x21;
    cfg.protect.compensation_config[4] = 0x04;
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_COMPENSATION_CONFIG);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT8(BB_TPS546_W_BLOCK5, w->width);
    TEST_ASSERT_EQUAL_UINT8(0x12, w->block[0]);
    TEST_ASSERT_EQUAL_UINT8(0x04, w->block[4]);
}

// Build program with positive vout_exp to exercise tps546_float_2_ulinear16
// exp_n >= 0 branch inside bb_power_tps546_program.c's compilation unit.
// exp_n=0 → raw = value / 2^0 = value (integer truncation).
void test_program_vout_command_positive_exp(void)
{
    bb_power_tps546_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.target_mv = 1200; // 1.2 V; with exp_n=0: raw = 1200/1000 = 1 (truncated)
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, (int8_t)0, prog, 40);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_VOUT_COMMAND);
    TEST_ASSERT_NOT_NULL(w);
    // exp_n=0: raw = 1.2 / 1.0 = 1 (truncated to uint16)
    TEST_ASSERT_EQUAL_UINT16(1u, w->word);
}

// Default freq when switch_freq_khz=0
void test_program_default_freq_when_zero(void)
{
    bb_power_tps546_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.target_mv = 1150;
    // switch_freq_khz=0 → default 650

    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_FREQUENCY_SWITCH);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT16(650u, w->word);
}

// Default OC limit when oc_limit_a=0
void test_program_default_oc_limit_when_zero(void)
{
    bb_power_tps546_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.target_mv = 1150;
    cfg.oc_response = 0xC0;
    // oc_limit_a=0 → default 30 A

    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_IOUT_OC_FAULT_LIMIT);
    TEST_ASSERT_NOT_NULL(w);
    int ma = tps546_slinear11_to_ma(w->word);
    TEST_ASSERT_INT_WITHIN(100, 30000, ma);
}

// ---------------------------------------------------------------------------
// essential flag: essential=true for core writes, essential=false for protection
// ---------------------------------------------------------------------------

// FREQUENCY_SWITCH, VOUT_COMMAND, IOUT_OC_FAULT_LIMIT, IOUT_OC_FAULT_RESPONSE
// must all have essential=true (a NACK on these means the chip cannot operate).
void test_program_essential_writes_have_essential_true(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);

    const bb_tps546_write_t *w;

    w = find_write(prog, n, BB_PMBUS_FREQUENCY_SWITCH);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_TRUE(w->essential);

    w = find_write(prog, n, BB_PMBUS_VOUT_COMMAND);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_TRUE(w->essential);

    w = find_write(prog, n, BB_PMBUS_IOUT_OC_FAULT_LIMIT);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_TRUE(w->essential);

    w = find_write(prog, n, BB_PMBUS_IOUT_OC_FAULT_RESPONSE);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_TRUE(w->essential);
}

// Protection-config writes (VOUT_OV_WARN, VOUT_UV_WARN, VIN_ON, VIN_OV_FAULT, etc.)
// must have essential=false (a NACK is warn+continue, not a fatal error).
void test_program_protection_writes_have_essential_false(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);

    const bb_tps546_write_t *w;

    w = find_write(prog, n, BB_PMBUS_VOUT_OV_WARN_LIMIT);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_FALSE(w->essential);

    w = find_write(prog, n, BB_PMBUS_VOUT_UV_WARN_LIMIT);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_FALSE(w->essential);

    w = find_write(prog, n, BB_PMBUS_VIN_ON);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_FALSE(w->essential);

    w = find_write(prog, n, BB_PMBUS_VIN_OV_FAULT_LIMIT);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_FALSE(w->essential);

    w = find_write(prog, n, BB_PMBUS_ON_OFF_CONFIG);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_FALSE(w->essential);
}

// TON_DELAY is written (and has essential=false) when ton_delay_ms is non-zero.
void test_program_ton_delay_written_when_nonzero(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    cfg.protect.ton_delay_ms = 5;

    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_TON_DELAY);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT8(BB_TPS546_W_WORD, w->width);
    TEST_ASSERT_FALSE(w->essential);
    // 5 ms < 1024 → SLINEAR11 exp=0, mantissa=5 → code=5
    TEST_ASSERT_EQUAL_UINT16(5u, w->word);
}

// TON_MAX_FAULT_LIMIT is written when ton_max_fault_ms is non-zero.
void test_program_ton_max_fault_written_when_nonzero(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    cfg.protect.ton_max_fault_ms = 10;

    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_TON_MAX_FAULT_LIMIT);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT8(BB_TPS546_W_WORD, w->width);
    TEST_ASSERT_FALSE(w->essential);
}

// TON_MAX_FAULT_RESPONSE is written when non-zero.
void test_program_ton_max_fault_response_written_when_nonzero(void)
{
    bb_power_tps546_cfg_t cfg = make_default_family_cfg();
    cfg.protect.ton_max_fault_response = 0xB0u;

    bb_tps546_write_t prog[40];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 40);
    const bb_tps546_write_t *w = find_write(prog, n, BB_PMBUS_TON_MAX_FAULT_RESPONSE);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT8(BB_TPS546_W_BYTE, w->width);
    TEST_ASSERT_EQUAL_UINT8(0xB0u, (uint8_t)w->word);
    TEST_ASSERT_FALSE(w->essential);
}

// Buffer overflow at emit_byte_ex: force overflow on an essential byte write
// (IOUT_OC_FAULT_RESPONSE is written near the end; use a tiny buffer that fits
// everything before it but not it). The builder returns -1 on overflow.
void test_program_overflow_on_essential_byte_write(void)
{
    bb_power_tps546_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.target_mv  = 1150;
    cfg.switch_freq_khz = 650;
    cfg.oc_limit_a = 30;
    cfg.oc_response = 0xC0;
    // zero protect: program = FREQUENCY_SWITCH + VOUT_COMMAND + IOUT_OC_FAULT_LIMIT
    //               + IOUT_OC_FAULT_RESPONSE = 4 entries total.
    // Pass max=3 so it overflows at IOUT_OC_FAULT_RESPONSE (the essential byte write).
    bb_tps546_write_t prog[4];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 3);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

// Buffer overflow at emit_block5_ex: force overflow on the block5 write.
// Minimum program with compensation: ON_OFF_CONFIG(1) + SYNC_CONFIG(2) + FREQ(3)
// + COMPENSATION(4). Use max=3 so compensation overflows emit_block5_ex.
void test_program_overflow_on_block5_write(void)
{
    bb_power_tps546_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.target_mv   = 1150;
    cfg.switch_freq_khz = 650;
    cfg.oc_limit_a  = 30;
    cfg.oc_response = 0xC0;
    cfg.protect.on_off_config = 0x1Au;
    cfg.protect.sync_config   = 0x10u;
    // compensation non-zero so block5 is emitted
    cfg.protect.compensation_config[0] = 0x01u;
    // Program order: ON_OFF_CONFIG, SYNC_CONFIG, FREQUENCY_SWITCH, COMPENSATION_CONFIG.
    // With max=3 the 4th entry (compensation) overflows emit_block5_ex.
    bb_tps546_write_t prog[4];
    int n = bb_power_tps546_build_init_program(&cfg, EXP_N, prog, 3);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

#include "bb_power_tps546.h"
#include "bb_power_driver.h"
#include "tps546_decode.h"
#include "bb_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bb_power_tps546";

#define OPERATION_ON   0x80u

// PMBus STATUS registers (not in the public header — read-only diagnostic use only)
#define BB_PMBUS_STATUS_BYTE        0x78u
#define BB_PMBUS_STATUS_WORD        0x79u
#define BB_PMBUS_STATUS_VOUT        0x7Au
#define BB_PMBUS_STATUS_IOUT        0x7Bu
#define BB_PMBUS_STATUS_INPUT       0x7Cu
#define BB_PMBUS_STATUS_TEMPERATURE 0x7Du
#define BB_PMBUS_STATUS_CML         0x7Eu
#define BB_PMBUS_STATUS_MFR         0x80u

// Defaults (lifted from TM init params)
#define TPS546_DEFAULT_FREQ_KHZ   650u
#define TPS546_DEFAULT_OC_LIMIT_A  30u
#define TPS546_DEFAULT_OC_RESPONSE 0xC0u

// Max writes in init program (ON_OFF_CONFIG through TON_MAX_FAULT_RESPONSE = ~30 max)
#define INIT_PROG_MAX 40

typedef struct {
    i2c_master_dev_handle_t dev;
    int8_t vout_n; // VOUT_MODE exponent (negative)
    uint16_t last_status_word; // last STATUS_WORD logged; 0xFFFF = sentinel (never logged)
} tps546_state_t;

// ---------------------------------------------------------------------------
// PMBus helpers
// ---------------------------------------------------------------------------

static esp_err_t pmbus_read_byte(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, 100);
}

static esp_err_t pmbus_write_byte(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, 2, 100);
}

static esp_err_t pmbus_write_word(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = {reg, val & 0xFF, (val >> 8) & 0xFF};
    return i2c_master_transmit(dev, buf, 3, 100);
}

static esp_err_t pmbus_read_word(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *val)
{
    uint8_t buf[2];
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, buf, 2, 100);
    if (err == ESP_OK) {
        *val = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    }
    return err;
}

static esp_err_t pmbus_send_byte(i2c_master_dev_handle_t dev, uint8_t reg)
{
    return i2c_master_transmit(dev, &reg, 1, 100);
}

static esp_err_t pmbus_write_block5(i2c_master_dev_handle_t dev, uint8_t reg, const uint8_t *data)
{
    // SMBus block write: [reg, count, data...]
    uint8_t buf[7] = {reg, 5, data[0], data[1], data[2], data[3], data[4]};
    return i2c_master_transmit(dev, buf, 7, 100);
}

// ---------------------------------------------------------------------------
// Execute a build_init_program output on the device
// ---------------------------------------------------------------------------

static esp_err_t exec_program(i2c_master_dev_handle_t dev,
                               const bb_tps546_write_t *prog, int n)
{
    for (int i = 0; i < n; i++) {
        esp_err_t err;
        switch (prog[i].width) {
        case BB_TPS546_W_BYTE:
            err = pmbus_write_byte(dev, prog[i].reg, (uint8_t)prog[i].word);
            break;
        case BB_TPS546_W_WORD:
            err = pmbus_write_word(dev, prog[i].reg, prog[i].word);
            break;
        case BB_TPS546_W_BLOCK5:
            err = pmbus_write_block5(dev, prog[i].reg, prog[i].block);
            break;
        default:
            return ESP_ERR_INVALID_ARG; // LCOV_EXCL_LINE
        }
        if (err != ESP_OK) {
            if (prog[i].essential) {
                bb_log_e(TAG, "PMBus write reg=0x%02X failed (essential): %d", prog[i].reg, err);
                return err;
            }
            // Non-critical limit the chip declined (e.g. OV/UV *warn* thresholds
            // 0x42/0x43, which some TPS546 parts reject): it keeps its default and
            // operation/fault protection is unaffected. Info, not a warning.
            bb_log_i(TAG, "PMBus reg=0x%02X declined by chip; default kept (non-critical limit): %d",
                     prog[i].reg, err);
        }
    }
    return ESP_OK;
}

// Encode millivolts to ULINEAR16 using the VOUT_MODE exponent.
static uint16_t mv_to_ulinear16(uint16_t mv, int8_t vout_n)
{
    int shift = -vout_n;
    return (uint16_t)((uint32_t)mv * (1U << shift) / 1000);
}

// ---------------------------------------------------------------------------
// vtable ops
// ---------------------------------------------------------------------------

static int op_read_vout_mv(void *state)
{
    tps546_state_t *s = state;
    uint16_t raw;
    if (pmbus_read_word(s->dev, BB_PMBUS_READ_VOUT, &raw) != ESP_OK) return -1;
    return tps546_ulinear16_to_mv(raw, s->vout_n);
}

static int op_read_iout_ma(void *state)
{
    tps546_state_t *s = state;
    uint16_t raw;
    if (pmbus_read_word(s->dev, BB_PMBUS_READ_IOUT, &raw) != ESP_OK) return -1;
    return tps546_slinear11_to_ma(raw);
}

static int op_read_vin_mv(void *state)
{
    tps546_state_t *s = state;
    uint16_t raw;
    if (pmbus_read_word(s->dev, BB_PMBUS_READ_VIN, &raw) != ESP_OK) return -1;
    return tps546_slinear11_to_mv(raw);
}

static int op_read_temp_c(void *state)
{
    tps546_state_t *s = state;
    uint16_t raw;
    if (pmbus_read_word(s->dev, BB_PMBUS_READ_TEMPERATURE_1, &raw) != ESP_OK) return -1;
    return tps546_slinear11_to_c_int(raw);
}

static bb_err_t op_set_vout_mv(void *state, uint16_t mv)
{
    tps546_state_t *s = state;
    uint16_t code = mv_to_ulinear16(mv, s->vout_n);
    if (pmbus_write_word(s->dev, BB_PMBUS_VOUT_COMMAND, code) != ESP_OK)
        return BB_ERR_INVALID_STATE;
    bb_log_i(TAG, "VOUT_COMMAND=0x%04X (%u mV)", code, mv);
    return BB_OK;
}

// Read STATUS registers and emit an edge-triggered warning when STATUS_WORD changes.
// Best-effort: I2C errors are ignored. CLEAR_FAULTS is never issued here.
static void op_poll(void *state)
{
    tps546_state_t *s = state;
    uint16_t st_word = 0;
    if (pmbus_read_word(s->dev, BB_PMBUS_STATUS_WORD, &st_word) != ESP_OK) return;

    // Only log when the value changes from what we last logged (edge-trigger).
    // 0xFFFF sentinel ensures the first read always logs (even if STATUS_WORD == 0).
    if (st_word == s->last_status_word) return;
    s->last_status_word = st_word;

    if (st_word == 0) {
        // Fault cleared — log once so the "all clear" is visible in the log.
        bb_log_w(TAG, "TPS546 STATUS (op): WORD=0x0000 (cleared)");
        return;
    }

    uint8_t st_vout = 0, st_iout = 0, st_in = 0, st_temp = 0;
    pmbus_read_byte(s->dev, BB_PMBUS_STATUS_VOUT,        &st_vout);
    pmbus_read_byte(s->dev, BB_PMBUS_STATUS_IOUT,        &st_iout);
    pmbus_read_byte(s->dev, BB_PMBUS_STATUS_INPUT,       &st_in);
    pmbus_read_byte(s->dev, BB_PMBUS_STATUS_TEMPERATURE, &st_temp);
    bb_log_w(TAG, "TPS546 STATUS (op): WORD=0x%04X VOUT=0x%02X IOUT=0x%02X INPUT=0x%02X TEMP=0x%02X",
             st_word, st_vout, st_iout, st_in, st_temp);
}

static const bb_power_driver_t s_tps546_vtable = {
    .read_vout_mv = op_read_vout_mv,
    .read_iout_ma = op_read_iout_ma,
    .read_vin_mv  = op_read_vin_mv,
    .read_temp_c  = op_read_temp_c,
    .set_vout_mv  = op_set_vout_mv,
    .poll         = op_poll,
    .name         = "tps546",
};

// ---------------------------------------------------------------------------
// Public open
// ---------------------------------------------------------------------------

bb_err_t bb_power_tps546_open(const bb_power_tps546_cfg_t *cfg,
                               bb_power_handle_t *out)
{
    if (!cfg || !out) return BB_ERR_INVALID_ARG;

    bb_log_i(TAG, "opening TPS546 at addr=0x%02X target=%u mV", cfg->addr, cfg->target_mv);

    tps546_state_t *s = calloc(1, sizeof *s); // LCOV_EXCL_BR_LINE
    if (!s) return BB_ERR_NO_SPACE;           // LCOV_EXCL_LINE
    s->last_status_word = 0xFFFF; // sentinel: force log on first poll read

    // Add device to I2C bus
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = cfg->addr,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(cfg->bus, &dev_cfg, &s->dev);
    if (err != ESP_OK) {
        bb_log_e(TAG, "i2c_master_bus_add_device failed: %d", err);
        free(s);
        return err;
    }

    // Read VOUT_MODE to get exponent — first I2C op; failure here means the chip is not responding
    uint8_t vout_mode;
    err = pmbus_read_byte(s->dev, BB_PMBUS_VOUT_MODE, &vout_mode);
    if (err != ESP_OK) {
        bb_log_e(TAG, "VOUT_MODE read failed (chip not responding at 0x%02X): %d", cfg->addr, err);
        free(s);
        return err;
    }

    s->vout_n = (int8_t)(vout_mode & 0x1F);
    if (s->vout_n & 0x10) s->vout_n |= (int8_t)0xE0; // sign-extend from bit 4
    bb_log_i(TAG, "VOUT_MODE=0x%02X exponent=%d", vout_mode, s->vout_n);

    // Dump STATUS registers BEFORE CLEAR_FAULTS so latched fault bits are visible.
    // Reads are best-effort — failures set the field to 0xFF/0xFFFF; init continues.
    {
        uint16_t st_word = 0xFFFF;
        uint8_t  st_byte = 0xFF, st_vout = 0xFF, st_iout = 0xFF;
        uint8_t  st_in   = 0xFF, st_temp = 0xFF, st_cml  = 0xFF, st_mfr = 0xFF;
        pmbus_read_word(s->dev, BB_PMBUS_STATUS_WORD,        &st_word);
        pmbus_read_byte(s->dev, BB_PMBUS_STATUS_BYTE,        &st_byte);
        pmbus_read_byte(s->dev, BB_PMBUS_STATUS_VOUT,        &st_vout);
        pmbus_read_byte(s->dev, BB_PMBUS_STATUS_IOUT,        &st_iout);
        pmbus_read_byte(s->dev, BB_PMBUS_STATUS_INPUT,       &st_in);
        pmbus_read_byte(s->dev, BB_PMBUS_STATUS_TEMPERATURE, &st_temp);
        pmbus_read_byte(s->dev, BB_PMBUS_STATUS_CML,         &st_cml);
        pmbus_read_byte(s->dev, BB_PMBUS_STATUS_MFR,         &st_mfr);
        bb_log_w(TAG, "TPS546 STATUS @init: WORD=0x%04X BYTE=0x%02X VOUT=0x%02X IOUT=0x%02X"
                 " INPUT=0x%02X TEMP=0x%02X CML=0x%02X MFR=0x%02X",
                 st_word, st_byte, st_vout, st_iout, st_in, st_temp, st_cml, st_mfr);
    }

    // Build and execute the full protection init program
    bb_tps546_write_t prog[INIT_PROG_MAX];
    int n = bb_power_tps546_build_init_program(cfg, s->vout_n, prog, INIT_PROG_MAX);
    if (n < 0) { free(s); return ESP_ERR_NO_MEM; } // LCOV_EXCL_LINE

    err = exec_program(s->dev, prog, n);
    if (err != ESP_OK) { free(s); return err; }
    bb_log_i(TAG, "init program: %d writes", n);

    // Clear latched faults from prior boots
    err = pmbus_send_byte(s->dev, BB_PMBUS_CLEAR_FAULTS);
    if (err != ESP_OK) { free(s); return err; }

    // Power on
    err = pmbus_write_byte(s->dev, BB_PMBUS_OPERATION, OPERATION_ON);
    if (err != ESP_OK) { free(s); return err; }
    bb_log_i(TAG, "powered on at %u mV", cfg->target_mv);

    bb_err_t rc = bb_power_handle_create(&s_tps546_vtable, s, out);
    if (rc != BB_OK) free(s); // LCOV_EXCL_LINE
    return rc;
}

#include "bb_power_tps546.h"
#include "bb_power_driver.h"
#include "tps546_decode.h"
#include "bb_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bb_power_tps546";

// PMBus registers
#define PMBUS_CLEAR_FAULTS          0x03
#define PMBUS_OPERATION             0x01
#define PMBUS_VOUT_MODE             0x20
#define PMBUS_VOUT_COMMAND          0x21
#define PMBUS_FREQUENCY_SWITCH      0x33
#define PMBUS_IOUT_OC_FAULT_LIMIT   0x46
#define PMBUS_IOUT_OC_FAULT_RESPONSE 0x47
#define PMBUS_READ_VIN              0x88
#define PMBUS_READ_VOUT             0x8B
#define PMBUS_READ_IOUT             0x8C
#define PMBUS_READ_TEMPERATURE_1    0x8D

#define OPERATION_ON   0x80

// Defaults (lifted from TM init params)
#define TPS546_DEFAULT_FREQ_KHZ   650
#define TPS546_DEFAULT_OC_LIMIT_A  30
#define TPS546_DEFAULT_OC_RESPONSE 0xC0

typedef struct {
    i2c_master_dev_handle_t dev;
    int8_t vout_n; // VOUT_MODE exponent (negative)
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

// Encode a small positive integer as SLINEAR11 with exponent 0.
static uint16_t slinear11_encode_int(uint16_t value)
{
    return value & 0x07FFu;
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
    if (pmbus_read_word(s->dev, PMBUS_READ_VOUT, &raw) != ESP_OK) return -1;
    return tps546_ulinear16_to_mv(raw, s->vout_n);
}

static int op_read_iout_ma(void *state)
{
    tps546_state_t *s = state;
    uint16_t raw;
    if (pmbus_read_word(s->dev, PMBUS_READ_IOUT, &raw) != ESP_OK) return -1;
    return tps546_slinear11_to_ma(raw);
}

static int op_read_vin_mv(void *state)
{
    tps546_state_t *s = state;
    uint16_t raw;
    if (pmbus_read_word(s->dev, PMBUS_READ_VIN, &raw) != ESP_OK) return -1;
    return tps546_slinear11_to_mv(raw);
}

static int op_read_temp_c(void *state)
{
    tps546_state_t *s = state;
    uint16_t raw;
    if (pmbus_read_word(s->dev, PMBUS_READ_TEMPERATURE_1, &raw) != ESP_OK) return -1;
    return tps546_slinear11_to_c_int(raw);
}

static bb_err_t op_set_vout_mv(void *state, uint16_t mv)
{
    tps546_state_t *s = state;
    uint16_t code = mv_to_ulinear16(mv, s->vout_n);
    if (pmbus_write_word(s->dev, PMBUS_VOUT_COMMAND, code) != ESP_OK)
        return BB_ERR_INVALID_STATE;
    bb_log_i(TAG, "VOUT_COMMAND=0x%04X (%u mV)", code, mv);
    return BB_OK;
}

static const bb_power_driver_t s_tps546_vtable = {
    .read_vout_mv = op_read_vout_mv,
    .read_iout_ma = op_read_iout_ma,
    .read_vin_mv  = op_read_vin_mv,
    .read_temp_c  = op_read_temp_c,
    .set_vout_mv  = op_set_vout_mv,
    .name         = "tps546",
};

// ---------------------------------------------------------------------------
// Public open
// ---------------------------------------------------------------------------

bb_err_t bb_power_tps546_open(const bb_power_tps546_cfg_t *cfg,
                               bb_power_handle_t *out)
{
    if (!cfg || !out) return BB_ERR_INVALID_ARG;

    tps546_state_t *s = calloc(1, sizeof *s); // LCOV_EXCL_BR_LINE
    if (!s) return BB_ERR_NO_SPACE;           // LCOV_EXCL_LINE

    // Add device to I2C bus
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = cfg->addr,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(cfg->bus, &dev_cfg, &s->dev);
    if (err != ESP_OK) { free(s); return err; }

    // Read VOUT_MODE to get exponent
    uint8_t vout_mode;
    err = pmbus_read_byte(s->dev, PMBUS_VOUT_MODE, &vout_mode);
    if (err != ESP_OK) { free(s); return err; }

    s->vout_n = (int8_t)(vout_mode & 0x1F);
    if (s->vout_n & 0x10) s->vout_n |= (int8_t)0xE0; // sign-extend from bit 4
    bb_log_i(TAG, "VOUT_MODE=0x%02X exponent=%d", vout_mode, s->vout_n);

    // Set initial output voltage
    uint16_t code = mv_to_ulinear16(cfg->target_mv, s->vout_n);
    err = pmbus_write_word(s->dev, PMBUS_VOUT_COMMAND, code);
    if (err != ESP_OK) { free(s); return err; }
    bb_log_i(TAG, "VOUT_COMMAND=0x%04X (%u mV)", code, cfg->target_mv);

    // Switching frequency
    uint32_t freq_khz = cfg->switch_freq_khz ? cfg->switch_freq_khz : TPS546_DEFAULT_FREQ_KHZ;
    err = pmbus_write_word(s->dev, PMBUS_FREQUENCY_SWITCH,
                           slinear11_encode_int((uint16_t)freq_khz));
    if (err != ESP_OK) { free(s); return err; }
    bb_log_i(TAG, "switching freq %u kHz", (unsigned)freq_khz);

    // Overcurrent limit
    uint8_t oc_limit = cfg->oc_limit_a ? cfg->oc_limit_a : TPS546_DEFAULT_OC_LIMIT_A;
    err = pmbus_write_word(s->dev, PMBUS_IOUT_OC_FAULT_LIMIT,
                           slinear11_encode_int(oc_limit));
    if (err != ESP_OK) { free(s); return err; }

    // Overcurrent response
    uint8_t oc_resp = cfg->oc_response ? cfg->oc_response : TPS546_DEFAULT_OC_RESPONSE;
    err = pmbus_write_byte(s->dev, PMBUS_IOUT_OC_FAULT_RESPONSE, oc_resp);
    if (err != ESP_OK) { free(s); return err; }

    // Clear latched faults from prior boots
    err = pmbus_send_byte(s->dev, PMBUS_CLEAR_FAULTS);
    if (err != ESP_OK) { free(s); return err; }

    // Power on
    err = pmbus_write_byte(s->dev, PMBUS_OPERATION, OPERATION_ON);
    if (err != ESP_OK) { free(s); return err; }
    bb_log_i(TAG, "powered on at %u mV", cfg->target_mv);

    bb_err_t rc = bb_power_handle_create(&s_tps546_vtable, s, out);
    if (rc != BB_OK) free(s); // LCOV_EXCL_LINE
    return rc;
}

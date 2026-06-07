// bb_fan_emc2101 — ESP-IDF backend for the EMC2101 fan controller + temp sensor.
// Ported from TaipanMiner components/asic/src/emc2101.c; file-static singleton
// migrated to instance state struct (one handle per device).
#include "bb_fan_emc2101.h"
#include "bb_fan_driver.h"
#include "emc2101_decode.h"
#include "bb_log.h"
#include <stdlib.h>

static const char *TAG = "bb_fan_emc2101";

// EMC2101 registers
#define REG_INTERNAL_TEMP       0x00
#define REG_EXTERNAL_MSB        0x01
#define REG_CONFIG              0x03
#define REG_EXTERNAL_LSB        0x10
#define REG_IDEALITY_FACTOR     0x17
#define REG_BETA_COMPENSATION   0x18
#define REG_TACH_LSB            0x46
#define REG_TACH_MSB            0x47
#define REG_FAN_CONFIG          0x4A
#define REG_FAN_SETTING         0x4C

typedef struct {
    i2c_master_dev_handle_t dev;
    int duty_pct; // last duty pct set; -1 if not yet set
} emc2101_state_t;

// ---------------------------------------------------------------------------
// I2C helpers
// ---------------------------------------------------------------------------

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, 2, 100);
}

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, 100);
}

// ---------------------------------------------------------------------------
// vtable ops
// ---------------------------------------------------------------------------

static bb_err_t op_set_duty_pct(void *state, int pct)
{
    emc2101_state_t *s = state;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    uint8_t raw = emc2101_pct_to_duty(pct);
    esp_err_t err = reg_write(s->dev, REG_FAN_SETTING, raw);
    if (err != ESP_OK) return err;
    s->duty_pct = pct;
    return BB_OK;
}

static int op_get_duty_pct(void *state)
{
    return ((emc2101_state_t *)state)->duty_pct;
}

static int op_read_rpm(void *state)
{
    emc2101_state_t *s = state;
    uint8_t lsb, msb;
    if (reg_read(s->dev, REG_TACH_LSB, &lsb) != ESP_OK) return -1;
    if (reg_read(s->dev, REG_TACH_MSB, &msb) != ESP_OK) return -1;
    return emc2101_decode_rpm(lsb, msb);
}

static bb_err_t op_read_die_temp_c(void *state, float *out)
{
    emc2101_state_t *s = state;
    uint8_t msb, lsb;
    esp_err_t err;
    err = reg_read(s->dev, REG_EXTERNAL_MSB, &msb);
    if (err != ESP_OK) return err;
    err = reg_read(s->dev, REG_EXTERNAL_LSB, &lsb);
    if (err != ESP_OK) return err;
    *out = emc2101_decode_ext_temp(msb, lsb);
    return BB_OK;
}

static bb_err_t op_read_board_temp_c(void *state, float *out)
{
    emc2101_state_t *s = state;
    uint8_t val;
    esp_err_t err = reg_read(s->dev, REG_INTERNAL_TEMP, &val);
    if (err != ESP_OK) return err;
    *out = emc2101_decode_int_temp(val);
    return BB_OK;
}

static const bb_fan_driver_t s_emc2101_vtable = {
    .set_duty_pct     = op_set_duty_pct,
    .get_duty_pct     = op_get_duty_pct,
    .read_rpm         = op_read_rpm,
    .read_die_temp_c  = op_read_die_temp_c,
    .read_board_temp_c = op_read_board_temp_c,
    .name             = "emc2101",
};

// ---------------------------------------------------------------------------
// Public open
// ---------------------------------------------------------------------------

bb_err_t bb_fan_emc2101_open(const bb_fan_emc2101_cfg_t *cfg,
                              bb_fan_handle_t *out)
{
    if (!cfg || !out) return BB_ERR_INVALID_ARG;

    emc2101_state_t *s = calloc(1, sizeof *s); // LCOV_EXCL_BR_LINE
    if (!s) return BB_ERR_NO_SPACE;            // LCOV_EXCL_LINE

    s->duty_pct = -1;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = cfg->addr,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(cfg->bus, &dev_cfg, &s->dev);
    if (err != ESP_OK) { free(s); return err; }

    // Enable external diode, disable auto-fan
    err = reg_write(s->dev, REG_CONFIG, 0x04);
    if (err != ESP_OK) { free(s); return err; }

    // Fan: direct PWM mode, enable driver, ~22.5kHz, 2 tach pulses/rev
    err = reg_write(s->dev, REG_FAN_CONFIG, 0x23);
    if (err != ESP_OK) { free(s); return err; }

    // Optional ideality/beta compensation (0 = skip, e.g. bitaxe-403)
    if (cfg->ideality) {
        err = reg_write(s->dev, REG_IDEALITY_FACTOR, cfg->ideality);
        if (err != ESP_OK) { free(s); return err; }
    }
    if (cfg->beta) {
        err = reg_write(s->dev, REG_BETA_COMPENSATION, cfg->beta);
        if (err != ESP_OK) { free(s); return err; }
    }

    // Fail-safe: start at 100% until telemetry loop adjusts
    err = op_set_duty_pct(s, 100);
    if (err != ESP_OK) { free(s); return err; }

    bb_log_i(TAG, "initialized at addr=0x%02X", cfg->addr);

    bb_err_t rc = bb_fan_handle_create(&s_emc2101_vtable, s, out);
    if (rc != BB_OK) free(s); // LCOV_EXCL_LINE
    return rc;
}

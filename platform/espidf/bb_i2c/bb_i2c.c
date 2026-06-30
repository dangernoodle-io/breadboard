// bb_i2c ESP-IDF backend — wraps esp_driver_i2c (new master API).
#include "bb_i2c.h"
#include "bb_log.h"
#include "bb_mem.h"
#include "driver/i2c_master.h"
#include <stdlib.h>

static const char *TAG = "bb_i2c";

// Default timeout in ms for all I2C transactions.
#define BB_I2C_TIMEOUT_MS 100

// ---------------------------------------------------------------------------
// Concrete struct definitions (opaque in public header)
// ---------------------------------------------------------------------------

struct bb_i2c_bus_s {
    i2c_master_bus_handle_t handle;
    uint32_t                clk_hz; // default clock for devices that pass speed_hz=0
};

struct bb_i2c_dev_s {
    i2c_master_dev_handle_t handle;
};

// ---------------------------------------------------------------------------
// Bus
// ---------------------------------------------------------------------------

bb_err_t bb_i2c_bus_create(const bb_i2c_bus_config_t *cfg, bb_i2c_bus_t *out)
{
    if (!cfg || !out || cfg->clk_hz == 0) return BB_ERR_INVALID_ARG;

    struct bb_i2c_bus_s *bus = bb_calloc_prefer_spiram(1, sizeof *bus);
    if (!bus) return BB_ERR_NO_SPACE; // LCOV_EXCL_LINE

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port            = cfg->port,
        .sda_io_num          = cfg->sda_gpio,
        .scl_io_num          = cfg->scl_gpio,
        .clk_source          = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt   = 7,
        .flags.enable_internal_pullup = cfg->internal_pullup,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus->handle);
    if (err != ESP_OK) {
        bb_log_e(TAG, "i2c_new_master_bus failed: %d", err);
        bb_mem_free(bus);
        return err;
    }

    bus->clk_hz = cfg->clk_hz;
    *out = bus;
    return BB_OK;
}

void bb_i2c_bus_delete(bb_i2c_bus_t bus)
{
    if (!bus) return;
    i2c_del_master_bus(bus->handle);
    bb_mem_free(bus);
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

bb_err_t bb_i2c_dev_add(bb_i2c_bus_t bus, uint8_t addr7, uint32_t speed_hz,
                         bb_i2c_dev_t *out)
{
    if (!bus || !out) return BB_ERR_INVALID_ARG;

    struct bb_i2c_dev_s *dev = bb_calloc_prefer_spiram(1, sizeof *dev);
    if (!dev) return BB_ERR_NO_SPACE; // LCOV_EXCL_LINE

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr7,
        .scl_speed_hz    = speed_hz ? speed_hz : bus->clk_hz,
    };

    esp_err_t err = i2c_master_bus_add_device(bus->handle, &dev_cfg, &dev->handle);
    if (err != ESP_OK) {
        bb_log_e(TAG, "i2c_master_bus_add_device failed: %d", err);
        bb_mem_free(dev);
        return err;
    }

    *out = dev;
    return BB_OK;
}

void bb_i2c_dev_remove(bb_i2c_dev_t dev)
{
    if (!dev) return;
    i2c_master_bus_rm_device(dev->handle);
    bb_mem_free(dev);
}

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------

bb_err_t bb_i2c_dev_write(bb_i2c_dev_t dev, const uint8_t *buf, size_t len)
{
    if (!dev || !buf || len == 0) return BB_ERR_INVALID_ARG;
    esp_err_t err = i2c_master_transmit(dev->handle, buf, len, BB_I2C_TIMEOUT_MS);
    if (err != ESP_OK) bb_log_e(TAG, "write failed: %d", err);
    return err;
}

bb_err_t bb_i2c_dev_read(bb_i2c_dev_t dev, uint8_t *buf, size_t len)
{
    if (!dev || !buf || len == 0) return BB_ERR_INVALID_ARG;
    esp_err_t err = i2c_master_receive(dev->handle, buf, len, BB_I2C_TIMEOUT_MS);
    if (err != ESP_OK) bb_log_e(TAG, "read failed: %d", err);
    return err;
}

bb_err_t bb_i2c_dev_write_read(bb_i2c_dev_t dev,
                                const uint8_t *w, size_t wlen,
                                uint8_t       *r, size_t rlen)
{
    if (!dev || !w || wlen == 0 || !r || rlen == 0) return BB_ERR_INVALID_ARG;
    esp_err_t err = i2c_master_transmit_receive(dev->handle, w, wlen, r, rlen,
                                                 BB_I2C_TIMEOUT_MS);
    if (err != ESP_OK) bb_log_e(TAG, "write_read failed: %d", err);
    return err;
}

bb_err_t bb_i2c_dev_read_reg8(bb_i2c_dev_t dev, uint8_t reg, uint8_t *val)
{
    if (!dev || !val) return BB_ERR_INVALID_ARG;
    return bb_i2c_dev_write_read(dev, &reg, 1, val, 1);
}

bb_err_t bb_i2c_dev_write_reg8(bb_i2c_dev_t dev, uint8_t reg, uint8_t val)
{
    if (!dev) return BB_ERR_INVALID_ARG;
    uint8_t buf[2] = {reg, val};
    return bb_i2c_dev_write(dev, buf, 2);
}

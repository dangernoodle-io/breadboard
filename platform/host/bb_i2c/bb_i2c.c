// bb_i2c host backend — injectable mock for tests.
// Maintains per-device state so register read/write round-trips are verifiable.
#include "bb_i2c.h"
#include "bb_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bb_i2c_host";

// ---------------------------------------------------------------------------
// Mock state (file-scope, reset via bb_i2c_host_reset)
// ---------------------------------------------------------------------------

#define BB_I2C_TEST_BUF_CAP 64

#ifdef BB_I2C_TESTING
#include "bb_i2c_test.h"

static uint8_t  s_read_queue[BB_I2C_TEST_BUF_CAP];
static size_t   s_read_queue_len;
static uint8_t  s_last_write[BB_I2C_TEST_BUF_CAP];
static size_t   s_last_write_len;
static bb_err_t s_forced_err;   // BB_OK means no forced error

void bb_i2c_host_reset(void)
{
    memset(s_read_queue,  0, sizeof s_read_queue);
    memset(s_last_write,  0, sizeof s_last_write);
    s_read_queue_len = 0;
    s_last_write_len = 0;
    s_forced_err     = BB_OK;
}

void bb_i2c_host_queue_read(const uint8_t *bytes, size_t len)
{
    if (!bytes) return;
    size_t n = len < BB_I2C_TEST_BUF_CAP ? len : BB_I2C_TEST_BUF_CAP;
    memcpy(s_read_queue, bytes, n);
    s_read_queue_len = n;
}

size_t bb_i2c_host_last_write(uint8_t *out, size_t max)
{
    if (!out || max == 0) return 0;
    size_t n = s_last_write_len < max ? s_last_write_len : max;
    memcpy(out, s_last_write, n);
    return n;
}

void bb_i2c_host_force_err(bb_err_t err)
{
    s_forced_err = err;
}

// Internal helpers used by transfers below.
static bb_err_t mock_check_err(void)
{
    if (s_forced_err != BB_OK) {
        bb_err_t e = s_forced_err;
        s_forced_err = BB_OK; // consume once
        return e;
    }
    return BB_OK;
}

static void mock_record_write(const uint8_t *buf, size_t len)
{
    size_t n = len < BB_I2C_TEST_BUF_CAP ? len : BB_I2C_TEST_BUF_CAP;
    memcpy(s_last_write, buf, n);
    s_last_write_len = n;
}

static void mock_do_read(uint8_t *buf, size_t len)
{
    size_t n = len < s_read_queue_len ? len : s_read_queue_len;
    memcpy(buf, s_read_queue, n);
    // zero remaining bytes if caller requested more than queued
    if (n < len) memset(buf + n, 0, len - n);
}

#else /* !BB_I2C_TESTING */

// Non-testing build: no-op stubs so the code compiles
static bb_err_t mock_check_err(void) { return BB_OK; }
static void mock_record_write(const uint8_t *buf, size_t len) { (void)buf; (void)len; }
static void mock_do_read(uint8_t *buf, size_t len) { memset(buf, 0, len); }

#endif /* BB_I2C_TESTING */

// ---------------------------------------------------------------------------
// Concrete struct definitions (opaque in public header)
// ---------------------------------------------------------------------------

struct bb_i2c_bus_s {
    uint32_t clk_hz;
};

struct bb_i2c_dev_s {
    uint8_t  addr7;
    uint32_t speed_hz; // effective speed (bus clk_hz if device passed 0)
};

// ---------------------------------------------------------------------------
// Bus
// ---------------------------------------------------------------------------

bb_err_t bb_i2c_bus_create(const bb_i2c_bus_config_t *cfg, bb_i2c_bus_t *out)
{
    if (!cfg || !out || cfg->clk_hz == 0) return BB_ERR_INVALID_ARG;

    struct bb_i2c_bus_s *bus = calloc(1, sizeof *bus);
    if (!bus) return BB_ERR_NO_SPACE; // LCOV_EXCL_LINE

    bus->clk_hz = cfg->clk_hz;
    *out = bus;
    bb_log_d(TAG, "bus created clk_hz=%" PRIu32, cfg->clk_hz);
    return BB_OK;
}

void bb_i2c_bus_delete(bb_i2c_bus_t bus)
{
    if (!bus) return;
    free(bus);
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

bb_err_t bb_i2c_dev_add(bb_i2c_bus_t bus, uint8_t addr7, uint32_t speed_hz,
                         bb_i2c_dev_t *out)
{
    if (!bus || !out) return BB_ERR_INVALID_ARG;

    struct bb_i2c_dev_s *dev = calloc(1, sizeof *dev);
    if (!dev) return BB_ERR_NO_SPACE; // LCOV_EXCL_LINE

    dev->addr7     = addr7;
    dev->speed_hz  = speed_hz ? speed_hz : bus->clk_hz;
    *out = dev;
    bb_log_d(TAG, "dev added addr=0x%02X speed_hz=%" PRIu32, addr7, dev->speed_hz);
    return BB_OK;
}

void bb_i2c_dev_remove(bb_i2c_dev_t dev)
{
    if (!dev) return;
    free(dev);
}

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------

bb_err_t bb_i2c_dev_write(bb_i2c_dev_t dev, const uint8_t *buf, size_t len)
{
    if (!dev || !buf || len == 0) return BB_ERR_INVALID_ARG;
    bb_err_t err = mock_check_err();
    if (err != BB_OK) return err;
    mock_record_write(buf, len);
    return BB_OK;
}

bb_err_t bb_i2c_dev_read(bb_i2c_dev_t dev, uint8_t *buf, size_t len)
{
    if (!dev || !buf || len == 0) return BB_ERR_INVALID_ARG;
    bb_err_t err = mock_check_err();
    if (err != BB_OK) return err;
    mock_do_read(buf, len);
    return BB_OK;
}

bb_err_t bb_i2c_dev_write_read(bb_i2c_dev_t dev,
                                const uint8_t *w, size_t wlen,
                                uint8_t       *r, size_t rlen)
{
    if (!dev || !w || wlen == 0 || !r || rlen == 0) return BB_ERR_INVALID_ARG;
    bb_err_t err = mock_check_err();
    if (err != BB_OK) return err;
    mock_record_write(w, wlen);
    mock_do_read(r, rlen);
    return BB_OK;
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

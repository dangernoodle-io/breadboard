// Tests for bb_i2c portable I2C bus+device abstraction.
#include "unity.h"
#include "bb_i2c.h"
#include "bb_i2c_test.h"
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bb_i2c_bus_config_t make_bus_cfg(void)
{
    bb_i2c_bus_config_t cfg = {
        .port           = 0,
        .sda_gpio       = 4,
        .scl_gpio       = 5,
        .clk_hz         = 400000,
        .internal_pullup = false,
    };
    return cfg;
}

// Create a bus + device, return them. Caller frees.
static void open_bus_dev(bb_i2c_bus_t *bus_out, bb_i2c_dev_t *dev_out)
{
    bb_i2c_bus_config_t cfg = make_bus_cfg();
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_bus_create(&cfg, bus_out));
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_add(*bus_out, 0x48, 0, dev_out));
}

// ---------------------------------------------------------------------------
// bus_create arg validation
// ---------------------------------------------------------------------------

void test_bb_i2c_bus_create_null_cfg(void)
{
    bb_i2c_bus_t bus;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_bus_create(NULL, &bus));
}

void test_bb_i2c_bus_create_null_out(void)
{
    bb_i2c_bus_config_t cfg = make_bus_cfg();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_bus_create(&cfg, NULL));
}

void test_bb_i2c_bus_create_zero_clk(void)
{
    bb_i2c_bus_config_t cfg = make_bus_cfg();
    cfg.clk_hz = 0;
    bb_i2c_bus_t bus;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_bus_create(&cfg, &bus));
}

void test_bb_i2c_bus_create_succeeds(void)
{
    bb_i2c_bus_config_t cfg = make_bus_cfg();
    bb_i2c_bus_t bus;
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_bus_create(&cfg, &bus));
    TEST_ASSERT_NOT_NULL(bus);
    bb_i2c_bus_delete(bus);
}

// ---------------------------------------------------------------------------
// bus_delete null safety
// ---------------------------------------------------------------------------

void test_bb_i2c_bus_delete_null_is_safe(void)
{
    bb_i2c_bus_delete(NULL); // must not crash
}

// ---------------------------------------------------------------------------
// dev_add arg validation
// ---------------------------------------------------------------------------

void test_bb_i2c_dev_add_null_bus(void)
{
    bb_i2c_dev_t dev;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_dev_add(NULL, 0x48, 0, &dev));
}

void test_bb_i2c_dev_add_null_out(void)
{
    bb_i2c_bus_config_t cfg = make_bus_cfg();
    bb_i2c_bus_t bus;
    bb_i2c_bus_create(&cfg, &bus);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_dev_add(bus, 0x48, 0, NULL));
    bb_i2c_bus_delete(bus);
}

void test_bb_i2c_dev_add_succeeds(void)
{
    bb_i2c_bus_config_t cfg = make_bus_cfg();
    bb_i2c_bus_t bus;
    bb_i2c_bus_create(&cfg, &bus);

    bb_i2c_dev_t dev;
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_add(bus, 0x48, 0, &dev));
    TEST_ASSERT_NOT_NULL(dev);

    bb_i2c_dev_remove(dev);
    bb_i2c_bus_delete(bus);
}

void test_bb_i2c_dev_add_explicit_speed(void)
{
    bb_i2c_bus_config_t cfg = make_bus_cfg();
    bb_i2c_bus_t bus;
    bb_i2c_bus_create(&cfg, &bus);

    bb_i2c_dev_t dev;
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_add(bus, 0x50, 100000, &dev));
    TEST_ASSERT_NOT_NULL(dev);

    bb_i2c_dev_remove(dev);
    bb_i2c_bus_delete(bus);
}

// ---------------------------------------------------------------------------
// dev_remove null safety
// ---------------------------------------------------------------------------

void test_bb_i2c_dev_remove_null_is_safe(void)
{
    bb_i2c_dev_remove(NULL); // must not crash
}

// ---------------------------------------------------------------------------
// write
// ---------------------------------------------------------------------------

void test_bb_i2c_dev_write_null_dev(void)
{
    uint8_t buf[1] = {0xAB};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_dev_write(NULL, buf, 1));
}

void test_bb_i2c_dev_write_null_buf(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_dev_write(dev, NULL, 1));
    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

void test_bb_i2c_dev_write_zero_len(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);
    uint8_t buf[1] = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_dev_write(dev, buf, 0));
    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

void test_bb_i2c_dev_write_records_payload(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);

    uint8_t tx[] = {0x01, 0x02, 0x03};
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_write(dev, tx, sizeof tx));

    uint8_t got[8];
    size_t n = bb_i2c_host_last_write(got, sizeof got);
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(tx, got, 3);

    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

// ---------------------------------------------------------------------------
// read
// ---------------------------------------------------------------------------

void test_bb_i2c_dev_read_null_dev(void)
{
    uint8_t buf[1];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_dev_read(NULL, buf, 1));
}

void test_bb_i2c_dev_read_null_buf(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_dev_read(dev, NULL, 1));
    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

void test_bb_i2c_dev_read_zero_len(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);
    uint8_t buf[1];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_dev_read(dev, buf, 0));
    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

void test_bb_i2c_dev_read_returns_queued_bytes(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);

    uint8_t queued[] = {0xDE, 0xAD, 0xBE, 0xEF};
    bb_i2c_host_queue_read(queued, sizeof queued);

    uint8_t got[4];
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_read(dev, got, sizeof got));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(queued, got, 4);

    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

void test_bb_i2c_dev_read_zeros_when_no_queue(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);

    uint8_t got[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_read(dev, got, sizeof got));
    for (size_t i = 0; i < 4; i++) TEST_ASSERT_EQUAL_UINT8(0, got[i]);

    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

// ---------------------------------------------------------------------------
// write_read
// ---------------------------------------------------------------------------

void test_bb_i2c_dev_write_read_null_dev(void)
{
    uint8_t w[1] = {0}, r[1];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_dev_write_read(NULL, w, 1, r, 1));
}

void test_bb_i2c_dev_write_read_null_w(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);
    uint8_t r[1];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_dev_write_read(dev, NULL, 1, r, 1));
    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

void test_bb_i2c_dev_write_read_null_r(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);
    uint8_t w[1] = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_dev_write_read(dev, w, 1, NULL, 1));
    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

void test_bb_i2c_dev_write_read_zero_wlen(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);
    uint8_t w[1] = {0}, r[1];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_dev_write_read(dev, w, 0, r, 1));
    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

void test_bb_i2c_dev_write_read_zero_rlen(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);
    uint8_t w[1] = {0}, r[1];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_dev_write_read(dev, w, 1, r, 0));
    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

void test_bb_i2c_dev_write_read_roundtrip(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);

    uint8_t queued[] = {0xCA, 0xFE};
    bb_i2c_host_queue_read(queued, sizeof queued);

    uint8_t w[] = {0x10, 0x20};
    uint8_t r[2];
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_write_read(dev, w, sizeof w, r, sizeof r));

    // write payload captured
    uint8_t wgot[8];
    size_t n = bb_i2c_host_last_write(wgot, sizeof wgot);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(w, wgot, 2);

    // read payload returned
    TEST_ASSERT_EQUAL_UINT8_ARRAY(queued, r, 2);

    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

// ---------------------------------------------------------------------------
// read_reg8
// ---------------------------------------------------------------------------

void test_bb_i2c_dev_read_reg8_null_dev(void)
{
    uint8_t val;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_dev_read_reg8(NULL, 0x00, &val));
}

void test_bb_i2c_dev_read_reg8_null_val(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_dev_read_reg8(dev, 0x01, NULL));
    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

void test_bb_i2c_dev_read_reg8_sends_reg_gets_value(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);

    uint8_t queued[] = {0x42};
    bb_i2c_host_queue_read(queued, 1);

    uint8_t val;
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_read_reg8(dev, 0xAB, &val));

    // Check the register byte was written
    uint8_t wbuf[8];
    size_t n = bb_i2c_host_last_write(wbuf, sizeof wbuf);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT8(0xAB, wbuf[0]);

    // Check the queued value was returned
    TEST_ASSERT_EQUAL_UINT8(0x42, val);

    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

// ---------------------------------------------------------------------------
// write_reg8
// ---------------------------------------------------------------------------

void test_bb_i2c_dev_write_reg8_null_dev(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_i2c_dev_write_reg8(NULL, 0x01, 0xFF));
}

void test_bb_i2c_dev_write_reg8_sends_reg_and_val(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);

    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_write_reg8(dev, 0x10, 0x55));

    uint8_t wbuf[8];
    size_t n = bb_i2c_host_last_write(wbuf, sizeof wbuf);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT8(0x10, wbuf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x55, wbuf[1]);

    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

// ---------------------------------------------------------------------------
// Register read/write round-trip
// ---------------------------------------------------------------------------

void test_bb_i2c_register_roundtrip(void)
{
    // Write reg 0x20 = 0xBB, then read it back via a queued response.
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);

    // Write phase
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_write_reg8(dev, 0x20, 0xBB));

    uint8_t wbuf[8];
    size_t n = bb_i2c_host_last_write(wbuf, sizeof wbuf);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT8(0x20, wbuf[0]);
    TEST_ASSERT_EQUAL_UINT8(0xBB, wbuf[1]);

    // Read phase: queue the value we "wrote"
    uint8_t queued[] = {0xBB};
    bb_i2c_host_queue_read(queued, 1);

    uint8_t val;
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_read_reg8(dev, 0x20, &val));

    // Check the register address was sent correctly
    n = bb_i2c_host_last_write(wbuf, sizeof wbuf);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT8(0x20, wbuf[0]);

    // Check the value matches
    TEST_ASSERT_EQUAL_UINT8(0xBB, val);

    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

// ---------------------------------------------------------------------------
// force_err propagation
// ---------------------------------------------------------------------------

void test_bb_i2c_force_err_write(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);

    bb_i2c_host_force_err(BB_ERR_TIMEOUT);
    uint8_t buf[] = {0x01};
    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, bb_i2c_dev_write(dev, buf, 1));

    // Error is consumed; next op should succeed
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_write(dev, buf, 1));

    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

void test_bb_i2c_force_err_read(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);

    bb_i2c_host_force_err(BB_ERR_INVALID_STATE);
    uint8_t buf[1];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_i2c_dev_read(dev, buf, 1));

    // Cleared; next succeeds
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_read(dev, buf, 1));

    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

void test_bb_i2c_force_err_write_read(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);

    bb_i2c_host_force_err(BB_ERR_NOT_FOUND);
    uint8_t w[1] = {0}, r[1];
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_i2c_dev_write_read(dev, w, 1, r, 1));

    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

void test_bb_i2c_force_err_read_reg8(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);

    bb_i2c_host_force_err(BB_ERR_TIMEOUT);
    uint8_t val;
    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, bb_i2c_dev_read_reg8(dev, 0x00, &val));

    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

void test_bb_i2c_force_err_write_reg8(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);

    bb_i2c_host_force_err(BB_ERR_INVALID_STATE);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_i2c_dev_write_reg8(dev, 0x01, 0xFF));

    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

void test_bb_i2c_force_err_clear_with_bb_ok(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);

    bb_i2c_host_force_err(BB_ERR_TIMEOUT);
    bb_i2c_host_force_err(BB_OK); // clear it
    uint8_t buf[] = {0x01};
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_write(dev, buf, 1));

    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

// ---------------------------------------------------------------------------
// host_reset clears all state
// ---------------------------------------------------------------------------

void test_bb_i2c_host_reset_clears_state(void)
{
    bb_i2c_bus_t bus; bb_i2c_dev_t dev;
    open_bus_dev(&bus, &dev);

    uint8_t queued[] = {0xFF};
    bb_i2c_host_queue_read(queued, 1);
    bb_i2c_host_force_err(BB_ERR_TIMEOUT);

    bb_i2c_host_reset();

    // After reset: read returns zero, not the queued byte
    uint8_t got[1] = {0xAA};
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_read(dev, got, 1));
    TEST_ASSERT_EQUAL_UINT8(0, got[0]);

    // After reset: no forced error
    uint8_t tx[1] = {0x01};
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_write(dev, tx, 1));

    bb_i2c_dev_remove(dev); bb_i2c_bus_delete(bus);
}

// ---------------------------------------------------------------------------
// Multiple devices on same bus
// ---------------------------------------------------------------------------

void test_bb_i2c_multiple_devices_on_bus(void)
{
    bb_i2c_bus_config_t cfg = make_bus_cfg();
    bb_i2c_bus_t bus;
    bb_i2c_bus_create(&cfg, &bus);

    bb_i2c_dev_t dev1, dev2;
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_add(bus, 0x48, 0, &dev1));
    TEST_ASSERT_EQUAL(BB_OK, bb_i2c_dev_add(bus, 0x49, 0, &dev2));
    TEST_ASSERT_NOT_NULL(dev1);
    TEST_ASSERT_NOT_NULL(dev2);
    TEST_ASSERT_TRUE(dev1 != dev2);

    bb_i2c_dev_remove(dev1);
    bb_i2c_dev_remove(dev2);
    bb_i2c_bus_delete(bus);
}

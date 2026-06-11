// bb_i2c_test.h — injectable mock hooks for bb_i2c host backend.
// Only available when BB_I2C_TESTING is defined.
#pragma once

#ifdef BB_I2C_TESTING

#include <stddef.h>
#include <stdint.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Reset all mock state (queued reads, last write, forced error).
// Call from setUp() to isolate tests.
void bb_i2c_host_reset(void);

// Queue bytes to be returned by the next read / write_read / read_reg8 call.
// At most BB_I2C_TEST_BUF_CAP bytes are stored; excess is silently dropped.
// If fewer bytes are queued than the read requests, the remainder is zeroed.
void bb_i2c_host_queue_read(const uint8_t *bytes, size_t len);

// Copy the payload from the last write / write_read / write_reg8 into out[0..max-1].
// Returns the number of bytes actually written (min of actual payload and max).
size_t bb_i2c_host_last_write(uint8_t *out, size_t max);

// Make the next I2C operation return err (any bb_err_t value).
// Pass BB_OK to clear the forced error (operations succeed normally).
void bb_i2c_host_force_err(bb_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* BB_I2C_TESTING */

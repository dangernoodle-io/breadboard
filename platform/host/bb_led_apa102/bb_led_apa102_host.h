#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns ptr to the most recently flushed buffer for the given handle ID slot.
// Caller doesn't own; pointer is valid until the next flush or close.
// Returns NULL if no flush has occurred or slot not active.
const uint8_t *bb_led_apa102_host_last_buf(int slot, size_t *out_len);

void bb_led_apa102_host_reset(void);

#ifdef __cplusplus
}
#endif

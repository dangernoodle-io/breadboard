#pragma once
#include "bb_button.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Test helper: inject a raw edge into the host driver.
// Updates the internal raw_pressed state and calls bb_button_dispatch_raw
// with the given now_ms timestamp. Use bb_button_set_mock_time_ms() (or pass
// now_ms directly) to control time in tests.
void bb_button_host_inject_edge(bb_button_handle_t h, bool pressed, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

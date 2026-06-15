#pragma once

#include <stdbool.h>

/**
 * Host-only test hook: inject the ota_validated field returned by
 * bb_board_get_info().  Default: false (mimics a zeroed host stub).
 * Only available when BB_BOARD_TESTING is defined.
 */
#ifdef BB_BOARD_TESTING
void bb_board_test_set_ota_validated(bool validated);
#endif

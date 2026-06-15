#pragma once

#include <stdbool.h>

/**
 * Host-only test hook: inject the has-IP state reported by bb_wifi_has_ip().
 * Default state: false (no IP, mimics disconnected device).
 * Only available when BB_WIFI_TESTING is defined.
 */
#ifdef BB_WIFI_TESTING
void bb_wifi_test_set_has_ip(bool has_ip);
#endif

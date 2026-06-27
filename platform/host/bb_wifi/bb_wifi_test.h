#pragma once

#include <stdbool.h>

/**
 * Host-only test hooks for bb_wifi.
 * Only available when BB_WIFI_TESTING is defined.
 */
#ifdef BB_WIFI_TESTING
void bb_wifi_test_set_has_ip(bool has_ip);
void bb_wifi_test_set_recovery_blocked(bool blocked);
int  bb_wifi_test_get_recovery_count(void);
const char *bb_wifi_test_get_last_recovery_reason(void);
void bb_wifi_test_reset_recovery(void);
#endif

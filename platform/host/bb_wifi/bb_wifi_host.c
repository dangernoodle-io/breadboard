#include "bb_wifi.h"

#ifndef ESP_PLATFORM

// Stub implementations for non-ESP platforms.
// DHCP hostname configuration is not available on host/Arduino.
// Still validates input; network operations return BB_OK (no-op).

bb_err_t bb_wifi_set_hostname(const char *hostname)
{
    if (!hostname || !*hostname) return BB_ERR_INVALID_ARG;
    return BB_OK;
}

#endif /* !ESP_PLATFORM */

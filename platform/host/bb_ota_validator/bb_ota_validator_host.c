#include "bb_ota_validator.h"

#ifndef ESP_PLATFORM

// Stub implementations for non-ESP platforms.
// OTA partition state is not available on host/Arduino; pending is always false.

bb_err_t bb_ota_mark_valid(const char *reason)
{
    (void)reason;
    return BB_ERR_INVALID_STATE;
}

bool bb_ota_is_pending(void)
{
    return false;
}

#endif /* !ESP_PLATFORM */

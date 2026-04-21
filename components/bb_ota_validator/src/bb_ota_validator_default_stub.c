#include "bb_ota_validator.h"

#ifndef ESP_PLATFORM

// Stub implementations for non-ESP platforms.
// Consumers on Arduino/host can provide their own strategy or accept these defaults.

bool bb_ota_default_is_pending(void)
{
    return false;
}

bb_err_t bb_ota_default_mark_valid(const char *reason)
{
    (void)reason;  // unused
    return BB_ERR_INVALID_STATE;
}

#endif /* !ESP_PLATFORM */

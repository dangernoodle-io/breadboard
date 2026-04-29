#pragma once

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Start NTP/SNTP client. Idempotent.
bb_err_t bb_ntp_start(const char *server);

/// Stop the NTP client. Idempotent.
bb_err_t bb_ntp_stop(void);

/// Returns true if clock has been synchronized at least once since start.
bool bb_ntp_is_synced(void);

#ifdef __cplusplus
}
#endif

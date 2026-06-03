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

/// Block until the clock is synced (and the wall clock reads a sane post-2023
/// epoch) or until timeout_ms elapses. Returns true if synced, false on
/// timeout. Polls with a short delay; call bb_ntp_start() first. Intended for
/// TLS-before-clock paths (e.g. OTA-only boot mode) where a 1970 epoch would
/// make server certs read as not-yet-valid.
bool bb_ntp_wait_synced(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

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

/// Returns the Unix timestamp (seconds since epoch) of the last successful NTP
/// sync, or 0 if the clock has never been synchronized.
int64_t bb_ntp_last_sync_unix(void);

/// Persist a POSIX timezone string via bb_nv_config_set_timezone AND apply it
/// immediately: setenv("TZ", posix_tz, 1) + tzset(). NULL or empty string
/// clears to UTC (applies "UTC0"). localtime() reflects the new zone
/// immediately after this call returns BB_OK.
bb_err_t bb_ntp_set_timezone(const char *posix_tz);

/// Read the saved timezone from bb_nv_config_timezone() and apply it with
/// setenv/tzset. When the stored value is empty, applies "UTC0" as the default.
/// Call this once at boot (e.g. from bb_ntp_start) before any wall-clock use.
void bb_ntp_apply_saved_timezone(void);

#ifdef __cplusplus
}
#endif

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bb_ntp_info — satellite component that registers a /api/info extender
 * reporting NTP sync state from bb_ntp.
 *
 * Include this component (REQUIRES bb_ntp_info) when you want the /api/info
 * response to include a nested "ntp" object:
 *   { "synced": bool, "last_sync_unix": number }
 *
 * Call bb_ntp_register_info() before bb_http_server_start (before the
 * extender table is frozen).
 *
 * "synced" reflects bb_ntp_is_synced(). "last_sync_unix" is the Unix
 * timestamp of the last successful sync (0 if never synced). UTC only;
 * no timezone conversion is performed here.
 *
 * Presence of this satellite component in the build (via REQUIRES) is the
 * opt-in mechanism — no Kconfig gate is needed.
 */
void bb_ntp_register_info(void);

#ifdef __cplusplus
}
#endif

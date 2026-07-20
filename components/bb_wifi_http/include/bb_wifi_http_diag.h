#pragma once

// bb_wifi_http_diag — the "wifi" bb_diag section (GET /api/diag/wifi),
// B1-1077 PR-3a. Replaces the prior hand-rolled exact route (diag_wifi_handler
// in platform/espidf/bb_wifi_http/bb_wifi_http_routes.c, removed this PR)
// with a bb_diag_register_section() fill adapter. GET /api/wifi and POST
// /api/scan are UNCHANGED -- only the diag snapshot moves.
//
// reason_histogram (B1-1077 PR-3a fork #3): N present-gated fixed fields,
// one per bb_wifi_disc_reason_t bucket (BB_WIFI_DISC_UNKNOWN..
// BB_WIFI_DISC_ASSOC_LEAVE), each omitted when its count is zero --
// byte-identical to the prior dynamic-key object (only non-zero buckets were
// ever emitted). NOT a dynamic-key object (bb_serialize has no such
// primitive) and NOT an emit-all-buckets shape (would be a visible wire
// change). `top_reason`/`top_reason_count` are always present, matching the
// prior handler.

#include "bb_diag_section.h"
#include "bb_serialize.h"
#include "bb_wifi.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// "reason_histogram" nested object -- one named field per non-sentinel
// bb_wifi_disc_reason_t bucket (present-gated, count > 0), plus the always-
// present top_reason/top_reason_count summary.
typedef struct {
    int64_t unknown;
    int64_t auth_fail;
    int64_t assoc_fail;
    int64_t handshake_timeout;
    int64_t connection_lost;
    int64_t no_ap_found;
    int64_t inactivity;
    int64_t deauth;
    int64_t beacon_timeout;
    int64_t bb_lost_ip;
    int64_t bb_egress_dead;
    int64_t bb_no_ip_watchdog;
    int64_t assoc_leave;
    bb_serialize_str_n_t top_reason;
    int64_t top_reason_count;
} bb_wifi_http_diag_hist_t;

// Section snapshot. Shared fields (ssid..disconnect_rssi) mirror
// bb_wifi_emit_section's GET /api/wifi shape; roam_count..has_ip are
// diag-only additions.
typedef struct {
    char                  ssid[33];
    char                  bssid[18];  // "aa:bb:cc:dd:ee:ff"
    int64_t               rssi;
    char                  ip[16];
    bool                  connected;
    bb_serialize_str_n_t  disc_reason;
    int64_t               disc_age_s;
    int64_t               retry_count;
    int64_t               restart_sta_count;
    int64_t               disconnect_rssi;
    int64_t               roam_count;
    int64_t               roam_age_s;
    int64_t               last_session_s;
    bb_serialize_str_n_t  net_mode;
    bool                  associated;
    bool                  has_ip;
    bb_wifi_http_diag_hist_t histogram;
} bb_wifi_http_diag_snap_t;

extern const bb_serialize_desc_t bb_wifi_http_diag_desc;

// Fill hook (bb_diag_fill_fn signature) -- pure/portable, built entirely
// from bb_wifi's public getters (all host + ESP-IDF backed). `args` is
// unused (this section declares no query_keys). Returns BB_ERR_INVALID_ARG
// if dst is NULL.
bb_err_t bb_wifi_http_diag_fill(void *dst, const bb_diag_fill_args_t *args);

#ifdef ESP_PLATFORM
// Registers this section as "wifi" (GET /api/diag/wifi) via
// bb_diag_register_section(). Composition-time-only, once.
// bbtool:init tier=regular fn=bb_wifi_http_diag_register
bb_err_t bb_wifi_http_diag_register(void);
#endif

#ifdef __cplusplus
}
#endif

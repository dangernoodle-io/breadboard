// bb_wifi_http_diag -- see bb_wifi_http_diag.h for the section contract.
// Pure/portable fill built entirely from bb_wifi's public getters
// (B1-1077 PR-3a).

#include "bb_wifi_http_diag.h"

#include "bb_wifi_http.h"
#include "bb_str.h"

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// reason_histogram present predicates -- one per non-sentinel
// bb_wifi_disc_reason_t bucket, gated on count > 0 (same "only non-zero
// buckets" contract as the prior dynamic-key handler).
// ---------------------------------------------------------------------------

static bool hist_unknown_present(const void *snap)
{
    return ((const bb_wifi_http_diag_hist_t *)snap)->unknown > 0;
}
static bool hist_auth_fail_present(const void *snap)
{
    return ((const bb_wifi_http_diag_hist_t *)snap)->auth_fail > 0;
}
static bool hist_assoc_fail_present(const void *snap)
{
    return ((const bb_wifi_http_diag_hist_t *)snap)->assoc_fail > 0;
}
static bool hist_handshake_timeout_present(const void *snap)
{
    return ((const bb_wifi_http_diag_hist_t *)snap)->handshake_timeout > 0;
}
static bool hist_connection_lost_present(const void *snap)
{
    return ((const bb_wifi_http_diag_hist_t *)snap)->connection_lost > 0;
}
static bool hist_no_ap_found_present(const void *snap)
{
    return ((const bb_wifi_http_diag_hist_t *)snap)->no_ap_found > 0;
}
static bool hist_inactivity_present(const void *snap)
{
    return ((const bb_wifi_http_diag_hist_t *)snap)->inactivity > 0;
}
static bool hist_deauth_present(const void *snap)
{
    return ((const bb_wifi_http_diag_hist_t *)snap)->deauth > 0;
}
static bool hist_beacon_timeout_present(const void *snap)
{
    return ((const bb_wifi_http_diag_hist_t *)snap)->beacon_timeout > 0;
}
static bool hist_bb_lost_ip_present(const void *snap)
{
    return ((const bb_wifi_http_diag_hist_t *)snap)->bb_lost_ip > 0;
}
static bool hist_bb_egress_dead_present(const void *snap)
{
    return ((const bb_wifi_http_diag_hist_t *)snap)->bb_egress_dead > 0;
}
static bool hist_bb_no_ip_watchdog_present(const void *snap)
{
    return ((const bb_wifi_http_diag_hist_t *)snap)->bb_no_ip_watchdog > 0;
}
static bool hist_assoc_leave_present(const void *snap)
{
    return ((const bb_wifi_http_diag_hist_t *)snap)->assoc_leave > 0;
}

// ---------------------------------------------------------------------------
// Descriptor
// ---------------------------------------------------------------------------

static const bb_serialize_field_t s_hist_fields[] = {
    { .key = "unknown", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_hist_t, unknown),
      .present = hist_unknown_present },
    { .key = "auth_fail", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_hist_t, auth_fail),
      .present = hist_auth_fail_present },
    { .key = "assoc_fail", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_hist_t, assoc_fail),
      .present = hist_assoc_fail_present },
    { .key = "handshake_timeout", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_hist_t, handshake_timeout),
      .present = hist_handshake_timeout_present },
    { .key = "connection_lost", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_hist_t, connection_lost),
      .present = hist_connection_lost_present },
    { .key = "no_ap_found", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_hist_t, no_ap_found),
      .present = hist_no_ap_found_present },
    { .key = "inactivity", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_hist_t, inactivity),
      .present = hist_inactivity_present },
    { .key = "deauth", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_hist_t, deauth),
      .present = hist_deauth_present },
    { .key = "beacon_timeout", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_hist_t, beacon_timeout),
      .present = hist_beacon_timeout_present },
    { .key = "bb_lost_ip", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_hist_t, bb_lost_ip),
      .present = hist_bb_lost_ip_present },
    { .key = "bb_egress_dead", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_hist_t, bb_egress_dead),
      .present = hist_bb_egress_dead_present },
    { .key = "bb_no_ip_watchdog", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_hist_t, bb_no_ip_watchdog),
      .present = hist_bb_no_ip_watchdog_present },
    { .key = "assoc_leave", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_hist_t, assoc_leave),
      .present = hist_assoc_leave_present },
    { .key = "top_reason", .type = BB_TYPE_STR_N,
      .offset = offsetof(bb_wifi_http_diag_hist_t, top_reason) },
    { .key = "top_reason_count", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_hist_t, top_reason_count) },
};

static const bb_serialize_field_t s_snap_fields[] = {
    { .key = "ssid", .type = BB_TYPE_STR,
      .offset = offsetof(bb_wifi_http_diag_snap_t, ssid),
      .max_len = sizeof(((bb_wifi_http_diag_snap_t *)0)->ssid) },
    { .key = "bssid", .type = BB_TYPE_STR,
      .offset = offsetof(bb_wifi_http_diag_snap_t, bssid),
      .max_len = sizeof(((bb_wifi_http_diag_snap_t *)0)->bssid) },
    { .key = "rssi", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_snap_t, rssi) },
    { .key = "ip", .type = BB_TYPE_STR,
      .offset = offsetof(bb_wifi_http_diag_snap_t, ip),
      .max_len = sizeof(((bb_wifi_http_diag_snap_t *)0)->ip) },
    { .key = "connected", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_wifi_http_diag_snap_t, connected) },
    { .key = "disc_reason", .type = BB_TYPE_STR_N,
      .offset = offsetof(bb_wifi_http_diag_snap_t, disc_reason) },
    { .key = "disc_age_s", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_snap_t, disc_age_s) },
    { .key = "retry_count", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_snap_t, retry_count) },
    { .key = "restart_sta_count", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_snap_t, restart_sta_count) },
    { .key = "disconnect_rssi", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_snap_t, disconnect_rssi) },
    { .key = "roam_count", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_snap_t, roam_count) },
    { .key = "roam_age_s", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_snap_t, roam_age_s) },
    { .key = "last_session_s", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_diag_snap_t, last_session_s) },
    { .key = "net_mode", .type = BB_TYPE_STR_N,
      .offset = offsetof(bb_wifi_http_diag_snap_t, net_mode) },
    { .key = "associated", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_wifi_http_diag_snap_t, associated) },
    { .key = "has_ip", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_wifi_http_diag_snap_t, has_ip) },
    { .key = "reason_histogram", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_wifi_http_diag_snap_t, histogram),
      .children = s_hist_fields,
      .n_children = sizeof(s_hist_fields) / sizeof(s_hist_fields[0]) },
};

const bb_serialize_desc_t bb_wifi_http_diag_desc = {
    .type_name = "wifi",
    .fields    = s_snap_fields,
    .n_fields  = sizeof(s_snap_fields) / sizeof(s_snap_fields[0]),
    .snap_size = sizeof(bb_wifi_http_diag_snap_t),
};

// ---------------------------------------------------------------------------
// Fill
// ---------------------------------------------------------------------------

bb_err_t bb_wifi_http_diag_fill(void *dst, const bb_diag_fill_args_t *args)
{
    (void)args;
    if (!dst) return BB_ERR_INVALID_ARG;

    bb_wifi_http_diag_snap_t *snap = (bb_wifi_http_diag_snap_t *)dst;
    memset(snap, 0, sizeof(*snap));

    bb_wifi_info_t info;
    bb_wifi_get_info(&info);

    // Shared fields -- SSOT with GET /api/wifi (bb_wifi_emit_section).
    bb_strlcpy(snap->ssid, info.ssid, sizeof(snap->ssid));
    bb_wifi_http_format_bssid(snap->bssid, info.bssid);
    snap->rssi = (int64_t)info.rssi;
    bb_strlcpy(snap->ip, info.ip, sizeof(snap->ip));
    snap->connected = info.connected;
    const char *disc_reason_str = bb_wifi_disc_reason_str(info.disc_reason);
    snap->disc_reason.ptr = disc_reason_str;
    snap->disc_reason.len = strlen(disc_reason_str);
    snap->disc_age_s          = (int64_t)info.disc_age_s;
    snap->retry_count         = (int64_t)info.retry_count;
    snap->restart_sta_count   = (int64_t)bb_wifi_get_restart_sta_count();
    snap->disconnect_rssi     = (int64_t)bb_wifi_get_disconnect_rssi();

    snap->roam_count     = (int64_t)bb_wifi_get_roam_count();
    snap->roam_age_s     = (int64_t)bb_wifi_get_roam_age_s();
    snap->last_session_s = (int64_t)bb_wifi_get_last_session_s();

    bool associated = bb_wifi_is_associated();
    bool has_ip     = bb_wifi_has_ip();
    bb_wifi_mode_t mode = bb_wifi_classify_mode(associated, has_ip);
    const char *net_mode_str = bb_wifi_mode_str(mode);
    snap->net_mode.ptr = net_mode_str;
    snap->net_mode.len = strlen(net_mode_str);
    snap->associated = associated;
    snap->has_ip     = has_ip;

    // Reason histogram.
    uint16_t hist[BB_WIFI_DISC_COUNT];
    bb_wifi_get_reason_histogram(hist, BB_WIFI_DISC_COUNT);
    uint16_t top_count = 0;
    bb_wifi_disc_reason_t top_reason = bb_wifi_reason_histogram_top(hist, &top_count);

    snap->histogram.unknown           = (int64_t)hist[BB_WIFI_DISC_UNKNOWN];
    snap->histogram.auth_fail         = (int64_t)hist[BB_WIFI_DISC_AUTH_FAIL];
    snap->histogram.assoc_fail        = (int64_t)hist[BB_WIFI_DISC_ASSOC_FAIL];
    snap->histogram.handshake_timeout = (int64_t)hist[BB_WIFI_DISC_HANDSHAKE_TIMEOUT];
    snap->histogram.connection_lost   = (int64_t)hist[BB_WIFI_DISC_CONNECTION_LOST];
    snap->histogram.no_ap_found       = (int64_t)hist[BB_WIFI_DISC_NO_AP_FOUND];
    snap->histogram.inactivity        = (int64_t)hist[BB_WIFI_DISC_INACTIVITY];
    snap->histogram.deauth            = (int64_t)hist[BB_WIFI_DISC_DEAUTH];
    snap->histogram.beacon_timeout    = (int64_t)hist[BB_WIFI_DISC_BEACON_TIMEOUT];
    snap->histogram.bb_lost_ip        = (int64_t)hist[BB_WIFI_DISC_BB_LOST_IP];
    snap->histogram.bb_egress_dead    = (int64_t)hist[BB_WIFI_DISC_BB_EGRESS_DEAD];
    snap->histogram.bb_no_ip_watchdog = (int64_t)hist[BB_WIFI_DISC_BB_NO_IP_WATCHDOG];
    snap->histogram.assoc_leave       = (int64_t)hist[BB_WIFI_DISC_ASSOC_LEAVE];

    const char *top_reason_str = bb_wifi_disc_reason_str(top_reason);
    snap->histogram.top_reason.ptr = top_reason_str;
    snap->histogram.top_reason.len = strlen(top_reason_str);
    snap->histogram.top_reason_count = (int64_t)top_count;

    return BB_OK;
}

#ifdef ESP_PLATFORM
bb_err_t bb_wifi_http_diag_register(void)
{
    bb_diag_section_t section = {
        .name         = "wifi",
        .desc         = "WiFi diagnostic surface (B1-969, rehomed from the dissolved bb_net_health)",
        .snap_desc    = &bb_wifi_http_diag_desc,
        .fill         = bb_wifi_http_diag_fill,
        .ctx          = NULL,
        .query_keys   = NULL,
        .n_query_keys = 0,
    };
    return bb_diag_register_section(&section);
}
#endif

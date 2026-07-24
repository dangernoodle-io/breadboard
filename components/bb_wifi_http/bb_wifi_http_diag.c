// bb_wifi_http_diag -- see bb_wifi_http_diag.h for the section contract.
// Pure/portable fill built entirely from bb_wifi's public getters
// (B1-1077 PR-3a).

#include "bb_wifi_http_diag.h"

#include "bb_http.h"
#include "bb_wifi_http.h"
#include "bb_wifi_http_common_priv.h"

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
// JSON Schema (B1-1180 PR-1) -- hand-authored, on-device (not host-gated;
// see bb_wifi_http_diag.h's doc comment). Its byte-fidelity against the
// BB_SERIALIZE_META_HOST-gated co-located meta table below is proven by
// test/test_host/test_bb_wifi_http_diag_meta_golden.c.
// ---------------------------------------------------------------------------

// A #define (not just the extern variable below) so the static-const
// describe route's response table (further down this file) can use the
// SAME literal text as a genuine compile-time constant expression --
// `.schema = bb_wifi_http_diag_schema` (the VARIABLE's runtime value) is
// NOT a valid static/file-scope initializer in C ("initializer element is
// not constant"); `.schema = BB_WIFI_HTTP_DIAG_SCHEMA_LITERAL` (the
// macro-expanded string literal) is.
#define BB_WIFI_HTTP_DIAG_SCHEMA_LITERAL \
    "{\"type\":\"object\",\"properties\":{" \
    "\"ssid\":{\"type\":\"string\"}," \
    "\"bssid\":{\"type\":\"string\"}," \
    "\"rssi\":{\"type\":\"integer\"}," \
    "\"ip\":{\"type\":\"string\"}," \
    "\"connected\":{\"type\":\"boolean\"}," \
    "\"disc_reason\":{\"type\":\"string\"}," \
    "\"disc_age_s\":{\"type\":\"integer\"}," \
    "\"retry_count\":{\"type\":\"integer\"}," \
    "\"restart_sta_count\":{\"type\":\"integer\"}," \
    "\"disconnect_rssi\":{\"type\":\"integer\"}," \
    "\"roam_count\":{\"type\":\"integer\"}," \
    "\"roam_age_s\":{\"type\":\"integer\"}," \
    "\"last_session_s\":{\"type\":\"integer\"}," \
    "\"net_mode\":{\"type\":\"string\"}," \
    "\"associated\":{\"type\":\"boolean\"}," \
    "\"has_ip\":{\"type\":\"boolean\"}," \
    "\"reason_histogram\":{\"type\":\"object\",\"properties\":{" \
    "\"unknown\":{\"type\":\"integer\"}," \
    "\"auth_fail\":{\"type\":\"integer\"}," \
    "\"assoc_fail\":{\"type\":\"integer\"}," \
    "\"handshake_timeout\":{\"type\":\"integer\"}," \
    "\"connection_lost\":{\"type\":\"integer\"}," \
    "\"no_ap_found\":{\"type\":\"integer\"}," \
    "\"inactivity\":{\"type\":\"integer\"}," \
    "\"deauth\":{\"type\":\"integer\"}," \
    "\"beacon_timeout\":{\"type\":\"integer\"}," \
    "\"bb_lost_ip\":{\"type\":\"integer\"}," \
    "\"bb_egress_dead\":{\"type\":\"integer\"}," \
    "\"bb_no_ip_watchdog\":{\"type\":\"integer\"}," \
    "\"assoc_leave\":{\"type\":\"integer\"}," \
    "\"top_reason\":{\"type\":\"string\"}," \
    "\"top_reason_count\":{\"type\":\"integer\"}}," \
    "\"required\":[],\"additionalProperties\":false}}," \
    "\"required\":[\"ssid\",\"bssid\",\"rssi\",\"ip\",\"connected\",\"disc_reason\"," \
    "\"disc_age_s\",\"retry_count\",\"restart_sta_count\",\"disconnect_rssi\"," \
    "\"roam_count\",\"roam_age_s\",\"last_session_s\",\"net_mode\",\"associated\"," \
    "\"has_ip\",\"reason_histogram\"]," \
    "\"additionalProperties\":false}"

const char *const bb_wifi_http_diag_schema = BB_WIFI_HTTP_DIAG_SCHEMA_LITERAL;

#if defined(BB_SERIALIZE_META_SHIP)

static const bb_serialize_field_meta_t s_wifi_http_diag_hist_meta_rows[] = {
    { .key = "unknown" },
    { .key = "auth_fail" },
    { .key = "assoc_fail" },
    { .key = "handshake_timeout" },
    { .key = "connection_lost" },
    { .key = "no_ap_found" },
    { .key = "inactivity" },
    { .key = "deauth" },
    { .key = "beacon_timeout" },
    { .key = "bb_lost_ip" },
    { .key = "bb_egress_dead" },
    { .key = "bb_no_ip_watchdog" },
    { .key = "assoc_leave" },
    { .key = "top_reason" },
    { .key = "top_reason_count" },
};

static const bb_serialize_field_meta_t s_wifi_http_diag_meta_rows[] = {
    { .key = "ssid",              .required = true },
    { .key = "bssid",             .required = true },
    { .key = "rssi",              .required = true },
    { .key = "ip",                .required = true },
    { .key = "connected",         .required = true },
    { .key = "disc_reason",       .required = true },
    { .key = "disc_age_s",        .required = true },
    { .key = "retry_count",       .required = true },
    { .key = "restart_sta_count", .required = true },
    { .key = "disconnect_rssi",   .required = true },
    { .key = "roam_count",        .required = true },
    { .key = "roam_age_s",        .required = true },
    { .key = "last_session_s",    .required = true },
    { .key = "net_mode",          .required = true },
    { .key = "associated",        .required = true },
    { .key = "has_ip",            .required = true },
    { .key = "reason_histogram",  .required = true,
      .children = s_wifi_http_diag_hist_meta_rows,
      .n_children = sizeof(s_wifi_http_diag_hist_meta_rows) / sizeof(s_wifi_http_diag_hist_meta_rows[0]) },
};

const bb_serialize_desc_meta_t bb_wifi_http_diag_meta = {
    .type_name = "wifi",
    .rows      = s_wifi_http_diag_meta_rows,
    .n_rows    = sizeof(s_wifi_http_diag_meta_rows) / sizeof(s_wifi_http_diag_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */

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

    // Shared fields -- SSOT with GET /api/wifi (bb_wifi_http_info_wire_fill,
    // bb_wifi_http_wire_priv.h), both routed through
    // bb_wifi_http_fill_common (B1-1106).
    bb_wifi_http_fill_common(snap->ssid, sizeof(snap->ssid), snap->bssid,
                              sizeof(snap->bssid), &snap->rssi, snap->ip,
                              sizeof(snap->ip), &snap->connected,
                              &snap->disc_reason, &snap->disc_age_s,
                              &snap->retry_count, &snap->restart_sta_count,
                              &snap->disconnect_rssi, &info);

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

// ---------------------------------------------------------------------------
// Describe-only route (B1-1180 PR-1 review fix) -- a PRODUCER-OWNED
// `static const` bb_route_t (handler=NULL), .rodata/flash, never DRAM. See
// bb_diag_section_t.describe_route's doc comment
// (components/bb_diag/include/bb_diag_section.h) for the full mechanism.
// ---------------------------------------------------------------------------

static const bb_route_response_t s_wifi_http_diag_describe_responses[] = {
    { .status = 200, .content_type = "application/json", .schema = BB_WIFI_HTTP_DIAG_SCHEMA_LITERAL },
    { .status = 0 },
};

static const bb_route_t s_wifi_http_diag_describe_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/wifi",
    .tag       = "diag",
    .summary   = "WiFi diagnostic surface (B1-969, rehomed from the dissolved bb_net_health)",
    .responses = s_wifi_http_diag_describe_responses,
    .handler   = NULL,
};

#ifdef ESP_PLATFORM
bb_err_t bb_wifi_http_diag_register(void)
{
    bb_diag_section_t section = {
        .name           = "wifi",
        .desc           = "WiFi diagnostic surface (B1-969, rehomed from the dissolved bb_net_health)",
        .snap_desc      = &bb_wifi_http_diag_desc,
        .fill           = bb_wifi_http_diag_fill,
        .ctx            = NULL,
        .query_keys     = NULL,
        .n_query_keys   = 0,
        .describe_route = &s_wifi_http_diag_describe_route,
    };
    return bb_diag_register_section(&section);
}
#endif

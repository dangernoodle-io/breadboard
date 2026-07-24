// bb_wifi_http_wire — v2 wire descriptor (SSOT) for the /api/wifi payload
// (B1-1057). Compiles on both host and ESP-IDF; no platform-specific code.
// See bb_wifi_http_wire_priv.h for the byte-fidelity contract.
//
// KB 820 (bb_wifi reason contract): "disc_reason" is a STABLE STRING label
// (e.g. "handshake_timeout", "bb_lost_ip") backed by the portable
// bb_wifi_disc_reason_t enum -- never a raw esp_wifi/backend-specific
// numeric code. See bb_wifi.h for the full enum + per-backend mapping
// (bb_wifi_map_esp_reason / bb_wifi_map_wl_status).

#include "bb_wifi_http_wire_priv.h"

#include "bb_wifi_http.h"
#include "bb_wifi_http_common_priv.h"

#include <stddef.h>

static const bb_serialize_field_t s_wifi_info_wire_fields[] = {
    { .key = "ssid", .type = BB_TYPE_STR,
      .offset = offsetof(bb_wifi_http_info_wire_t, ssid),
      .max_len = sizeof(((bb_wifi_http_info_wire_t *)0)->ssid) },
    { .key = "bssid", .type = BB_TYPE_STR,
      .offset = offsetof(bb_wifi_http_info_wire_t, bssid),
      .max_len = sizeof(((bb_wifi_http_info_wire_t *)0)->bssid) },
    { .key = "rssi", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_info_wire_t, rssi) },
    { .key = "ip", .type = BB_TYPE_STR,
      .offset = offsetof(bb_wifi_http_info_wire_t, ip),
      .max_len = sizeof(((bb_wifi_http_info_wire_t *)0)->ip) },
    { .key = "connected", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_wifi_http_info_wire_t, connected) },
    { .key = "disc_reason", .type = BB_TYPE_STR_N,
      .offset = offsetof(bb_wifi_http_info_wire_t, disc_reason) },
    { .key = "disc_age_s", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_info_wire_t, disc_age_s) },
    { .key = "retry_count", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_info_wire_t, retry_count) },
    { .key = "restart_sta_count", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_info_wire_t, restart_sta_count) },
    { .key = "disconnect_rssi", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_info_wire_t, disconnect_rssi) },
};

const bb_serialize_desc_t bb_wifi_http_info_wire_desc = {
    .type_name = "wifi_info",
    .fields    = s_wifi_info_wire_fields,
    .n_fields  = sizeof(s_wifi_info_wire_fields) / sizeof(s_wifi_info_wire_fields[0]),
    .snap_size = sizeof(bb_wifi_http_info_wire_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1059 PR-2a exemplar) -- co-located JSON
// Schema companion to bb_wifi_http_info_wire_desc above, gated behind
// BB_SERIALIZE_META_HOST (see bb_wifi_http_wire_priv.h's banner for the
// full mechanism doc). "required" here mirrors the "required" array of
// platform/espidf/bb_wifi_http/bb_wifi_http_routes.c's hand-authored
// k_wifi_info_schema literal (["ssid","connected"]) -- see
// test_bb_wifi_http_wire_meta_golden.c for the fidelity proof this table
// is meant to (eventually) replace that literal with.
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_SHIP)

static const bb_serialize_field_meta_t s_wifi_info_wire_meta_rows[] = {
    { .key = "ssid",              .required = true },
    { .key = "bssid" },
    { .key = "rssi" },
    { .key = "ip" },
    { .key = "connected",         .required = true },
    { .key = "disc_reason" },
    { .key = "disc_age_s" },
    { .key = "retry_count" },
    { .key = "restart_sta_count" },
    { .key = "disconnect_rssi" },
};

const bb_serialize_desc_meta_t bb_wifi_http_info_wire_meta = {
    .type_name = "wifi_info",
    .rows      = s_wifi_info_wire_meta_rows,
    .n_rows    = sizeof(s_wifi_info_wire_meta_rows) / sizeof(s_wifi_info_wire_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */

// ---------------------------------------------------------------------------
// GET /api/wifi response schema (B1-1059 emit batch B, site B1). Config OFF
// (default) keeps the pre-existing hand-authored literal, byte-identical --
// RELOCATED here verbatim from platform/espidf/bb_wifi_http/
// bb_wifi_http_routes.c's k_wifi_info_schema (a pure relocation: same const
// data, registered identically, no new runtime-compose symbols in this
// build variant). Config ON composes it at init from
// bb_wifi_http_info_wire_desc/bb_wifi_http_info_wire_meta via
// bb_serialize_meta_ensure_composed() -- see
// bb_wifi_http_info_wire_ensure_schema_patched() below. Accepted config-ON
// delta (see test_bb_wifi_http_wire_meta_golden.c): the composed body drops
// the embedded "title" (the meta engine has no descriptor-level title
// hook -- bb_openapi_register_schema()'s own "WifiInfo" component-name
// argument already carries that) and gains a trailing
// "additionalProperties":false (the engine always closes every rendered
// object) -- config-OFF ships the untouched literal.
// ---------------------------------------------------------------------------
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
// Sized with headroom over the golden-proven composed body
// (test_bb_wifi_http_wire_meta_golden.c's k_expected_meta_schema is 398
// bytes incl. NUL) -- any future desync trips bb_serialize_meta_openapi_
// schema()'s own bounded-buffer BB_ERR_NO_SPACE contract instead of
// silently truncating.
static char s_wifi_info_schema_buf[512];
#else
static const char k_wifi_info_schema[] =
    "{\"title\":\"WifiInfo\",\"type\":\"object\","
    "\"properties\":{"
    "\"ssid\":{\"type\":\"string\"},"
    "\"bssid\":{\"type\":\"string\"},"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"ip\":{\"type\":\"string\"},"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"disc_reason\":{\"type\":\"string\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"retry_count\":{\"type\":\"integer\"},"
    "\"restart_sta_count\":{\"type\":\"integer\"},"
    "\"disconnect_rssi\":{\"type\":\"integer\"}},"
    "\"required\":[\"ssid\",\"connected\"]}";
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
bb_err_t bb_wifi_http_info_wire_ensure_schema_patched(void)
{
    return bb_serialize_meta_ensure_composed(bb_serialize_meta_openapi_schema,
                                              &bb_wifi_http_info_wire_desc, &bb_wifi_http_info_wire_meta,
                                              s_wifi_info_schema_buf, sizeof(s_wifi_info_schema_buf));
}
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

const char *bb_wifi_http_info_wire_get_schema(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    return s_wifi_info_schema_buf;
#else
    return k_wifi_info_schema;
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */
}

void bb_wifi_http_info_wire_fill(bb_wifi_http_info_wire_t *dst, const bb_wifi_info_t *info)
{
    bb_wifi_http_fill_common(dst->ssid, sizeof(dst->ssid), dst->bssid,
                              sizeof(dst->bssid), &dst->rssi, dst->ip,
                              sizeof(dst->ip), &dst->connected,
                              &dst->disc_reason, &dst->disc_age_s,
                              &dst->retry_count, &dst->restart_sta_count,
                              &dst->disconnect_rssi, info);
}

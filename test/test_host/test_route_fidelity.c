// Fidelity audit: verify that each JSON handler's emitted body validates
// against the response schema declared alongside it.
//
// Pattern mirrors test_route_schemas.c (registry-seeding + declared fixtures),
// using the host backend's fake req cookie.
//
// SKIPPED ROUTES (with rationale):
//   /api/events            - SSE stream (text/event-stream); not JSON
//   /api/wifi/scan         - bb_wifi_routes.c includes <esp_wifi.h> which cannot
//                            link on host; scan uses esp_wifi_scan internally.
//                            Follow-up: add a bb_wifi_scan host shim (B1-???).
//   /api/update/apply      - ota_update_handler calls esp_restart() (ESP-IDF only);
//                            ota_boot_handler same. Both handlers are static in
//                            ESP-IDF platform files.
//   /api/update/partitions - calls esp_ota_get_running_partition (ESP-IDF only);
//                            the wire descriptor + copy_rows fn now have
//                            host coverage, see test_ota_validator_partitions_wire.c
//                            (same posture as /api/wifi/scan above)
//   /api/update/recover    - calls esp_ota_erase_last_boot_app_partition (ESP-IDF only)
//   /api/update/mark-valid (200) - bb_ota_is_pending() always false on host; 409 path covered
//   /api/diag/heap-check   - calls heap_caps_* (ESP-IDF only)
//   /api/diag/sockets      - walks LWIP TCP PCBs (ESP-IDF only)
//   /api/diag/tasks        - CONFIG_FREERTOS_USE_TRACE_FACILITY (ESP-IDF only)
//   /api/diag/coredump     - ESP-IDF partition API (ESP-IDF only)
//   /api/diag/panic/trigger- debug-only; ESP-IDF only
//   /api/openapi.json      - uses bb_openapi_emit_stream; no fixed schema to validate
//                            (self-describing meta-spec); already covered by test_openapi_emit.c
//   DELETE /api/diag/boot  - returns 204 No Content; no JSON body
//   POST /api/log/level    - returns 204 No Content on success; JSON only on 400 errors
//   /api/diag/partitions   - deleted (B1-1077 PR-3a); duplicated the shipped
//                            "storage/partitions" bb_diag section (see
//                            test_bb_diag_storage_partitions.c)
//   /api/diag/wifi          - moved to the generic bb_diag section registry
//                            (B1-1077 PR-3a); see test_bb_wifi_http_diag.c
//   /api/diag/websocket     - moved to the generic bb_diag section registry
//                            (B1-1077 PR-3a); see test_bb_ws_server_diag.c
//   /api/diag/rings         - moved to the generic bb_diag section registry
//                            (B1-1077 PR-3a); see test_bb_ring_diag.c
// REMOVED ROUTES (no longer registered):
//   /api/diag/events       - bb_event_routes/bb_event/bb_event_ring dissolved (B1-1045);
//                            /api/events is now served by bb_data_http (no diag
//                            equivalent yet -- rebuild tracked B1-1052)
//   /api/logs              - retired; use GET /api/events?topic=log (structured JSON)
//   /api/logs/status       - retired with /api/logs
//   /api/board             - dropped; was superseded by /api/info, itself since removed
//   /api/ping              - dropped; superseded by /api/health
//   /api/version           - dropped; was covered by /api/info's .version field
//   /api/info              - bb_info + its satellites deleted (B1-893); no consumer
//                            of the surviving fields (dupes elsewhere or dead)
//   /api/ota/check         - moved to POST /api/update/check
//   /api/ota/update        - moved to POST /api/update/apply
//   /api/ota/status        - moved to GET /api/update/progress
//   /api/ota/push          - moved to POST /api/update/push
//   /api/ota/mark-valid    - moved to POST /api/update/mark-valid
//   GET .../manifest       - the manifest-registration component (empty
//                            nvs/mdns arrays in every shipped build since
//                            B1-708 PR7's last registrant removal) deleted
//                            outright; zero external consumers

#include "unity.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_host.h"
#include "bb_openapi_validate_priv.h"
#include "bb_settings.h"
#include "bb_wifi.h"
#include "bb_wifi_http.h"
#include "bb_system.h"
#include "bb_mdns.h"
#include "bb_diag.h"
#include "bb_ota_check.h"
#include "bb_log.h"
#include "bb_clock.h"
#include "bb_timer.h"
#include "bb_health.h"

// bb_mdns_started and bb_mdns_get_hostname are declared in bb_mdns.h only
// under #ifdef ESP_PLATFORM. The host stub implements them; forward-declare
// them here so this file compiles on host builds.
bool bb_mdns_started(void);
const char *bb_mdns_get_hostname(void);

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

#include "cJSON.h"

#include "bb_http_serialize_stream.h"
#include "../../components/bb_wifi_http/bb_wifi_http_wire_priv.h"
#include "../../components/bb_health/bb_health_wire_priv.h"

// ---------------------------------------------------------------------------
// Production schemas (copied from production route descriptors)
// Any edit to the production literal must also update the copy here.
// ---------------------------------------------------------------------------

// POST /api/reboot — bb_system_routes.c
static const char k_reboot_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{\"status\":{\"type\":\"string\"}},"
    "\"required\":[\"status\"]}";

// GET /api/health — bb_health.c (espidf)
// TA-505: status bools/enums only. Numeric fields (rssi, disc_reason, etc.)
// moved to /api/diag/wifi. mdns KEPT (locked decision B1-269).
static const char k_health_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"ok\":{\"type\":\"boolean\"},"
    "\"network\":{\"type\":\"object\","
    "\"properties\":{"
    "\"ssid\":{\"type\":\"string\"},"
    "\"bssid\":{\"type\":\"string\"},"
    "\"ip\":{\"type\":\"string\"},"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"mdns\":{\"type\":[\"string\",\"null\"]}}}},"
    "\"required\":[\"ok\",\"network\"]}";

// GET /api/wifi — bb_wifi_routes.c (espidf); handler uses bb_wifi_get_info
// which now has a host stub.
static const char k_wifi_schema[] =
    "{\"type\":\"object\","
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

// GET /api/update/progress — bb_ota_pull.c (espidf)
static const char k_ota_status_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"state\":{\"type\":\"string\","
    "\"enum\":[\"idle\",\"checking\",\"downloading\",\"verifying\",\"complete\",\"error\"]},"
    "\"in_progress\":{\"type\":\"boolean\"},"
    "\"progress_pct\":{\"type\":\"integer\"},"
    "\"last_error\":{\"type\":\"string\"}},"
    "\"required\":[\"state\",\"in_progress\",\"progress_pct\"]}";

// POST /api/update/mark-valid 200 — bb_ota_validator.c (espidf)
static const char k_mark_valid_ok_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{\"status\":{\"type\":\"string\"}},"
    "\"required\":[\"status\"]}";

// POST /api/update/mark-valid 409 — bb_ota_validator.c (espidf)
static const char k_mark_valid_409_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{\"error\":{\"type\":\"string\"}},"
    "\"required\":[\"error\"]}";

// GET /api/diag/boot — platform/espidf/bb_diag_http/bb_diag_http_routes.c
static const char k_boot_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"reset_reason\":{\"type\":\"string\"},"
    "\"wdt_resets\":{\"type\":\"integer\"},"
    "\"panic\":{\"type\":\"object\","
    "\"properties\":{"
    "\"available\":{\"type\":\"boolean\"},"
    "\"boots_since\":{\"type\":\"integer\"}},"
    "\"required\":[\"available\"]}},"
    "\"required\":[\"reset_reason\",\"wdt_resets\",\"panic\",\"pending_verify\",\"rolled_back\"]}";

// GET /api/diag/panic — platform/espidf/bb_diag_http/bb_diag_http_routes.c
static const char k_panic_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"available\":{\"type\":\"boolean\"},"
    "\"boots_since\":{\"type\":\"integer\"},"
    "\"reset_reason\":{\"type\":\"string\"},"
    "\"log_tail\":{\"type\":\"string\"},"
    "\"task\":{\"type\":\"string\"},"
    "\"exc_pc\":{\"type\":\"integer\"},"
    "\"exc_cause\":{\"type\":\"integer\"},"
    "\"backtrace\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
    "\"panic_reason\":{\"type\":\"string\"},"
    "\"app_sha256\":{\"type\":\"string\"}},"
    "\"required\":[\"available\"]}";

// GET /api/update/status — platform/espidf/bb_ota_check/bb_ota_check_espidf.c
// The enum literal mirrors BB_OTA_CHECK_OUTCOME_ENUM_JSON (bb_ota_check.h) —
// keep byte-identical (B1-462a).
static const char k_update_status_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"current\":{\"type\":\"string\"},"
    "\"latest\":{\"type\":\"string\"},"
    "\"download_url\":{\"type\":\"string\"},"
    "\"available\":{\"type\":\"boolean\"},"
    "\"last_check_ok\":{\"type\":\"boolean\"},"
    "\"enabled\":{\"type\":\"boolean\"},"
    "\"outcome\":{\"type\":\"string\","
    "\"enum\":[" BB_OTA_CHECK_OUTCOME_ENUM_JSON "]},"
    "\"last_check_ts\":{\"type\":\"integer\"}},"
    "\"required\":[\"current\",\"latest\",\"download_url\","
    "\"available\",\"last_check_ok\",\"enabled\",\"outcome\"]}";

// GET /api/update/config — components/bb_ota_check/src/bb_ota_check_common.c
static const char k_update_config_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{\"enabled\":{\"type\":\"boolean\"}},"
    "\"required\":[\"enabled\"]}";

// POST /api/update/check — platform/espidf/bb_ota_pull/bb_ota_pull.c (ota_check_handler)
static const char k_update_check_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{\"status\":{\"type\":\"string\"}},"
    "\"required\":[\"status\"]}";

// PATCH /api/wifi 202 — platform/espidf/bb_wifi/bb_wifi_routes.c (CONFIG_BB_WIFI_RECONFIGURE)
static const char k_wifi_patch_202_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{\"status\":{\"type\":\"string\"}},"
    "\"required\":[\"status\"]}";

// PATCH /api/wifi 400 — platform/espidf/bb_wifi/bb_wifi_routes.c (CONFIG_BB_WIFI_RECONFIGURE)
static const char k_wifi_patch_400_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{\"error\":{\"type\":\"string\"}},"
    "\"required\":[\"error\"]}";

// PATCH /api/wifi 500 — platform/espidf/bb_wifi_http/bb_wifi_http_routes.c
// (CONFIG_BB_WIFI_RECONFIGURE; B1-1022 finding #2)
static const char k_wifi_patch_500_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{\"error\":{\"type\":\"string\"}},"
    "\"required\":[\"error\"]}";

// GET /api/diag/partitions and GET /api/diag/wifi fidelity coverage moved to
// the generic bb_diag section registry (B1-1077 PR-3a, #951) — see
// test_bb_diag_storage_partitions.c and test_bb_wifi_http_diag.c.

// GET /api/log/level — platform/espidf/bb_log_http/bb_log_http.c
static const char k_log_level_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"levels\":{\"type\":\"array\","
    "\"items\":{\"type\":\"string\","
    "\"enum\":[\"none\",\"error\",\"warn\",\"info\",\"debug\",\"verbose\"]}},"
    "\"tags\":{\"type\":\"array\","
    "\"items\":{\"type\":\"object\","
    "\"properties\":{"
    "\"tag\":{\"type\":\"string\"},"
    "\"level\":{\"type\":\"string\"}},"
    "\"required\":[\"tag\",\"level\"]}}},"
    "\"required\":[\"levels\",\"tags\"]}";

// ---------------------------------------------------------------------------
// Host-local handler implementations
// These mirror what the production handlers emit using host-available functions.
// Handlers are self-contained; they do not call bb_system_restart().
// ---------------------------------------------------------------------------

static bb_err_t h_reboot(bb_http_request_t *req)
{
    static const char body[] = "{\"status\":\"rebooting\"}";
    bb_http_resp_set_type(req, "application/json");
    bb_err_t err = bb_http_resp_send_chunk(req, body, sizeof(body) - 1);
    if (err != BB_OK) return err;
    return bb_http_resp_send_chunk(req, NULL, 0);
}

// B1-1149: calls the real production wire descriptor (bb_health_wire_desc,
// components/bb_health/bb_health_wire_priv.h) instead of hand-rolling
// bb_json, mirroring health_handler()'s own gather step
// (platform/espidf/bb_health/bb_health.c) with the host-available
// bb_wifi_get_info/bb_mdns_get_hostname. TA-505: status bools/enums only —
// no free_heap; numeric network fields moved to /api/diag/wifi.
// test_fidelity_health_no_raw_numbers catches any future numeric addition at
// test time. validated dropped (B1-977, bb_board dissolution). Renders only
// the ROOT slice (ok/network) -- exactly what k_health_schema declares;
// registered bb_health_section entries are a separate concern already
// covered by test/test_host/test_bb_health_compose.c, so this fixture
// deliberately does not touch the section registry (and stays independent
// of RUN_TEST ordering across that suite).
static bb_err_t h_health(bb_http_request_t *req)
{
    bb_health_wire_t root;
    memset(&root, 0, sizeof(root));

    root.ok = bb_health_compute_ok();

    bb_wifi_info_t info;
    bb_wifi_get_info(&info);
    strncpy(root.network.ssid, info.ssid, sizeof(root.network.ssid) - 1);
    root.network.ssid[sizeof(root.network.ssid) - 1] = '\0';
    bb_wifi_http_format_bssid(root.network.bssid, info.bssid);
    strncpy(root.network.ip, info.ip, sizeof(root.network.ip) - 1);
    root.network.ip[sizeof(root.network.ip) - 1] = '\0';
    root.network.connected = info.connected;

    const char *hostname = bb_mdns_get_hostname();
    root.network.mdns = hostname
        ? (bb_serialize_str_n_t){ .ptr = hostname, .len = strlen(hostname) }
        : (bb_serialize_str_n_t){ 0 };

    return bb_http_serialize_stream(req, &bb_health_wire_desc, &root);
}

// B1-467/B1-1057: calls the real production fill fn + wire descriptor
// (bb_wifi_http_info_wire_fill/bb_wifi_http_info_wire_desc,
// components/bb_wifi_http/bb_wifi_http_wire_priv.h) instead of hand-copying
// its fields, so this fidelity fixture exercises the SSOT directly. Mirrors
// the live-read fallback path in wifi_info_handler
// (platform/espidf/bb_wifi_http/bb_wifi_http_routes.c).
static bb_err_t h_wifi_info(bb_http_request_t *req)
{
    bb_wifi_info_t info;
    bb_wifi_get_info(&info);

    bb_wifi_http_info_wire_t snap;
    bb_wifi_http_info_wire_fill(&snap, &info);

    return bb_http_serialize_stream(req, &bb_wifi_http_info_wire_desc, &snap);
}

// OTA status: mirrors ota_status_handler idle-state path.
static bb_err_t h_ota_status(bb_http_request_t *req)
{
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "state", "idle");
    bb_http_resp_json_obj_set_bool(&obj, "in_progress", false);
    bb_http_resp_json_obj_set_num(&obj, "progress_pct", 0);
    bb_http_resp_json_obj_set_str(&obj, "last_error", "");
    return bb_http_resp_json_obj_end(&obj);
}

// GET /api/diag/boot — panic not available (clean boot)
static bb_err_t h_boot_no_panic(bb_http_request_t *req)
{
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "reset_reason", "power-on");
    bb_http_resp_json_obj_set_int(&obj, "wdt_resets", 0);
    bb_http_resp_json_obj_set_obj_begin(&obj, "panic");
    bb_http_resp_json_obj_set_bool(&obj, "available", false);
    bb_http_resp_json_obj_set_obj_end(&obj);
    bb_http_resp_json_obj_set_bool(&obj, "pending_verify", false);
    bb_http_resp_json_obj_set_bool(&obj, "rolled_back", false);
    return bb_http_resp_json_obj_end(&obj);
}

// GET /api/diag/boot — panic available (post-panic boot)
static bb_err_t h_boot_with_panic(bb_http_request_t *req)
{
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "reset_reason", "panic");
    bb_http_resp_json_obj_set_int(&obj, "wdt_resets", 3);
    bb_http_resp_json_obj_set_obj_begin(&obj, "panic");
    bb_http_resp_json_obj_set_bool(&obj, "available", true);
    bb_http_resp_json_obj_set_int(&obj, "boots_since", 0);
    bb_http_resp_json_obj_set_obj_end(&obj);
    bb_http_resp_json_obj_set_bool(&obj, "pending_verify", false);
    bb_http_resp_json_obj_set_bool(&obj, "rolled_back", false);
    return bb_http_resp_json_obj_end(&obj);
}

// OTA mark-valid 409: on host bb_ota_is_pending() is always false.
static bb_err_t h_ota_mark_valid_409(bb_http_request_t *req)
{
    bb_http_resp_set_type(req, "application/json");
    bb_http_resp_set_status(req, 409);
    static const char body[] = "{\"error\":\"not pending\"}";
    bb_err_t err = bb_http_resp_send_chunk(req, body, sizeof(body) - 1);
    if (err != BB_OK) return err;
    return bb_http_resp_send_chunk(req, NULL, 0);
}

// GET /api/diag/panic — no-panic path (host: bb_diag_panic_available() = false,
// bb_diag_panic_coredump_available() = false).
// Mirrors panic_get_handler in platform/espidf/bb_diag_http/bb_diag_http_routes.c.
static bb_err_t h_diag_panic(bb_http_request_t *req)
{
    bool available      = bb_diag_panic_available();
    bool coredump_avail = bb_diag_panic_coredump_available();

    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;

    bb_http_resp_json_obj_set_bool(&obj, "available", available);
    if (available || coredump_avail) {
        bb_http_resp_json_obj_set_int(&obj, "boots_since",
                                      (int64_t)bb_diag_panic_boots_since());
    }
    return bb_http_resp_json_obj_end(&obj);
}

// GET /api/update/status — idle state (mirrors status_handler in
// platform/espidf/bb_ota_check/bb_ota_check_espidf.c).
// Requires bb_ota_check_init() to have been called.
static bb_err_t h_update_status(bb_http_request_t *req)
{
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");

    bb_ota_check_status_t st;
    bb_err_t err = bb_ota_check_get_status(&st);
    if (err != BB_OK) {
        bb_http_resp_set_status(req, 503);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "not initialized");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    /* Map outcome enum to string (mirrors outcome_str() in production handler). */
    const char *outcome;
    switch (st.outcome) {
        case BB_OTA_CHECK_OUTCOME_UP_TO_DATE: outcome = "up_to_date";  break;
        case BB_OTA_CHECK_OUTCOME_AVAILABLE:  outcome = "available";   break;
        case BB_OTA_CHECK_OUTCOME_NO_ASSET:   outcome = "no_asset";    break;
        case BB_OTA_CHECK_OUTCOME_FAILED:     outcome = "check_failed";break;
        case BB_OTA_CHECK_OUTCOME_CHECK_ON_APPLY: outcome = "check_on_apply"; break;
        default:                           outcome = "unknown";     break;
    }

    bb_http_json_obj_stream_t obj;
    err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;
    bb_http_resp_json_obj_set_str(&obj,  "current",       st.current);
    bb_http_resp_json_obj_set_str(&obj,  "latest",        st.latest);
    bb_http_resp_json_obj_set_str(&obj,  "download_url",  st.download_url);
    bb_http_resp_json_obj_set_bool(&obj, "available",     st.available);
    bb_http_resp_json_obj_set_bool(&obj, "last_check_ok", st.last_check_ok);
    bb_http_resp_json_obj_set_bool(&obj, "enabled",       st.enabled);
    bb_http_resp_json_obj_set_str(&obj,  "outcome",       outcome);
    if (st.last_check_us != 0) {
        bb_http_resp_json_obj_set_int(&obj, "last_check_ts",
                                      (int64_t)(st.last_check_us / 1000000));
    }
    return bb_http_resp_json_obj_end(&obj);
}

// GET /api/update/config — mirrors bb_ota_check_config_get_handler in
// components/bb_ota_check/src/bb_ota_check_common.c.
static bb_err_t h_update_config_get(bb_http_request_t *req)
{
    bool enabled = bb_settings_update_check_enabled_get();
    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;
    bb_http_resp_json_obj_set_bool(&obj, "enabled", enabled);
    return bb_http_resp_json_obj_end(&obj);
}

// POST /api/update/check — mirrors ota_check_handler in
// platform/espidf/bb_ota_pull/bb_ota_pull.c.
// Handler just responds {"status":"checking"} and kicks the worker.
// On host we only test the JSON shape, not the kick.
static bb_err_t h_update_check(bb_http_request_t *req)
{
    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;
    bb_http_resp_json_obj_set_str(&obj, "status", "checking");
    return bb_http_resp_json_obj_end(&obj);
}

// GET /api/log/level — mirrors log_level_get_handler in
// platform/espidf/bb_log_http/bb_log_http.c.
// Uses only portable bb_log_tag_at / bb_log_level_to_str APIs.
static bb_err_t h_log_level_get(bb_http_request_t *req)
{
    static const char *s_level_names[] = {
        "none", "error", "warn", "info", "debug", "verbose",
    };

    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;

    bb_http_resp_json_obj_set_arr_begin(&obj, "levels");
    for (size_t i = 0; i < sizeof(s_level_names) / sizeof(s_level_names[0]); i++) {
        bb_http_resp_json_obj_set_str(&obj, NULL, s_level_names[i]);
    }
    bb_http_resp_json_obj_set_arr_end(&obj);

    bb_http_resp_json_obj_set_arr_begin(&obj, "tags");
    const char *tag = NULL;
    bb_log_level_t lv;
    for (size_t i = 0; bb_log_tag_at(i, &tag, &lv); i++) {
        bb_http_resp_json_obj_set_obj_begin(&obj, NULL);
        bb_http_resp_json_obj_set_str(&obj, "tag",   tag);
        bb_http_resp_json_obj_set_str(&obj, "level", bb_log_level_to_str(lv));
        bb_http_resp_json_obj_set_obj_end(&obj);
    }
    bb_http_resp_json_obj_set_arr_end(&obj);

    return bb_http_resp_json_obj_end(&obj);
}

// PATCH /api/wifi 202 — mirrors wifi_patch_handler success path.
static bb_err_t h_wifi_patch_202(bb_http_request_t *req)
{
    bb_http_resp_set_status(req, 202);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "status", "rebooting_to_try_wifi");
    return bb_http_resp_json_obj_end(&obj);
}

// PATCH /api/wifi 400 — mirrors wifi_patch_handler validation failure path
// (B1-1022: BB_ERR_VALIDATION/BB_ERR_INVALID_ARG/BB_ERR_UNSUPPORTED from
// bb_data_apply() all shape to this same body; the pre-cutover "ssid
// required" literal is gone -- see wifi_patch_handler's own comment on why
// BB_ERR_NOT_FOUND is deliberately excluded from this branch).
static bb_err_t h_wifi_patch_400(bb_http_request_t *req)
{
    bb_http_resp_set_status(req, 400);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "error", "invalid request body or credentials");
    return bb_http_resp_json_obj_end(&obj);
}

// PATCH /api/wifi 500 — mirrors wifi_patch_handler's fallthrough for any
// bb_err_t not in the 400 list (incl. BB_ERR_NOT_FOUND per B1-1022 finding
// #3 -- a composition-invariant violation, not client error).
static bb_err_t h_wifi_patch_500(bb_http_request_t *req)
{
    bb_http_resp_set_status(req, 500);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "error", "internal error");
    return bb_http_resp_json_obj_end(&obj);
}

// ---------------------------------------------------------------------------
// Audit entry: one per (handler, expected-status, schema) pair
// ---------------------------------------------------------------------------

typedef struct {
    const char              *route;
    bb_http_handler_fn       handler;
    int                      expected_status;
    const char              *expected_content_type;
    const char              *schema;
} fidelity_entry_t;

// Table of all audited (route, handler, status, content-type, schema) tuples.
// Entries that require subsystem state setup (bb_ota_check_init) are NOT
// listed here; their test functions below do their own setup and call
// run_fidelity with a stack-allocated entry.
static const fidelity_entry_t k_audit[] = {
    { "/api/reboot",            h_reboot,            200, "application/json", k_reboot_schema        },
    { "/api/health",            h_health,            200, "application/json", k_health_schema        },
    { "/api/wifi",              h_wifi_info,         200, "application/json", k_wifi_schema          },
    { "/api/update/progress",   h_ota_status,        200, "application/json", k_ota_status_schema    },
    { "/api/update/mark-valid", h_ota_mark_valid_409,409, "application/json", k_mark_valid_409_schema},
    { "/api/diag/boot (clean)", h_boot_no_panic,     200, "application/json", k_boot_schema          },
    { "/api/diag/boot (panic)", h_boot_with_panic,   200, "application/json", k_boot_schema          },
    /* No-state routes: */
    { "/api/diag/panic",        h_diag_panic,        200, "application/json", k_panic_schema         },
    { "/api/update/check",      h_update_check,      200, "application/json", k_update_check_schema    },
    { "/api/log/level",         h_log_level_get,     200, "application/json", k_log_level_schema       },
    { "PATCH /api/wifi 202",         h_wifi_patch_202,    202, "application/json", k_wifi_patch_202_schema  },
    { "PATCH /api/wifi 400",         h_wifi_patch_400,    400, "application/json", k_wifi_patch_400_schema  },
    { "PATCH /api/wifi 500",         h_wifi_patch_500,    500, "application/json", k_wifi_patch_500_schema  },
    { NULL, NULL, 0, NULL, NULL },
};

// ---------------------------------------------------------------------------
// Helper: run one fidelity entry
// ---------------------------------------------------------------------------

static void run_fidelity(const fidelity_entry_t *e)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    bb_err_t handler_rc = e->handler(req);
    (void)handler_rc;

    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_err_t end_rc = bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_EQUAL_MESSAGE(BB_OK, end_rc, e->route);

    // --- status check ---
    char msg[512];
    snprintf(msg, sizeof(msg), "%s: expected status %d got %d",
             e->route, e->expected_status, cap.status);
    TEST_ASSERT_EQUAL_INT_MESSAGE(e->expected_status, cap.status, msg);

    // --- content-type check ---
    snprintf(msg, sizeof(msg), "%s: expected content-type '%s' got '%s'",
             e->route, e->expected_content_type, cap.content_type);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(e->expected_content_type, cap.content_type, msg);

    // --- body must be non-empty ---
    snprintf(msg, sizeof(msg), "%s: body is empty", e->route);
    TEST_ASSERT_NOT_NULL_MESSAGE(cap.body, msg);
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, (int)cap.body_len, msg);

    // --- parse body ---
    cJSON *parsed = cJSON_Parse(cap.body);
    snprintf(msg, sizeof(msg), "%s: body is not valid JSON: %s",
             e->route, cap.body ? cap.body : "(null)");
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, msg);

    // --- validate against declared schema ---
    bb_openapi_validate_err_t verr;
    memset(&verr, 0, sizeof(verr));
    bb_err_t vrc = bb_openapi_validate(e->schema, parsed, &verr);
    if (vrc != BB_OK) {
        snprintf(msg, sizeof(msg),
                 "%s: schema violation at '%s': %s",
                 e->route, verr.path, verr.message);
        TEST_FAIL_MESSAGE(msg);
    }

    cJSON_Delete(parsed);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_fidelity_reboot(void)
{
    run_fidelity(&k_audit[0]);
}

void test_fidelity_health(void)
{
    run_fidelity(&k_audit[1]);
}

void test_fidelity_wifi_info(void)
{
    run_fidelity(&k_audit[2]);
}

void test_fidelity_ota_status(void)
{
    run_fidelity(&k_audit[3]);
}

void test_fidelity_ota_mark_valid_409(void)
{
    run_fidelity(&k_audit[4]);
}

void test_fidelity_boot_no_panic(void)
{
    run_fidelity(&k_audit[5]);
}

void test_fidelity_boot_with_panic(void)
{
    run_fidelity(&k_audit[6]);
}

void test_fidelity_diag_panic(void)
{
    run_fidelity(&k_audit[7]);
}

void test_fidelity_update_check(void)
{
    run_fidelity(&k_audit[8]);
}

void test_fidelity_log_level_get(void)
{
    run_fidelity(&k_audit[9]);
}

void test_fidelity_wifi_patch_202(void)
{
    run_fidelity(&k_audit[10]);
}

void test_fidelity_wifi_patch_400(void)
{
    run_fidelity(&k_audit[11]);
}

void test_fidelity_wifi_patch_500(void)
{
    run_fidelity(&k_audit[12]);
}

// Routes that require subsystem state setup are tested individually below.

// GET /api/update/status: requires bb_ota_check_init().
void test_fidelity_update_status(void)
{
    bb_ota_check_init(NULL);   /* idempotent; uses Kconfig defaults */

    const fidelity_entry_t e = {
        "/api/update/status", h_update_status, 200,
        "application/json",   k_update_status_schema,
    };
    run_fidelity(&e);
}

// GET /api/update/config: requires bb_settings_update_check_enabled_get() (portable).
void test_fidelity_update_config_get(void)
{
    const fidelity_entry_t e = {
        "/api/update/config", h_update_config_get, 200,
        "application/json",   k_update_config_schema,
    };
    run_fidelity(&e);
}

// ---------------------------------------------------------------------------
// CORS header capture tests
//
// CORS headers are set per-route, not globally. The three OTA routes that
// lacked ACAO/ACAPN now set them explicitly at the top of their handlers:
//   - GET  /api/update/status   (bb_ota_check_espidf.c: status_handler)
//   - POST /api/update/apply    (bb_ota_pull.c: ota_update_handler)
//   - GET  /api/update/progress (bb_ota_pull.c: ota_status_handler)
//
// Routes that already set ACAO themselves (TaipanMiner /api/stats etc.) are
// NOT touched — they continue to set it exactly once.
//
// These tests verify:
//   (a) the capture harness correctly tracks CORS header calls, and
//   (b) handlers that call set_header for ACAO and ACAPN produce
//       has_acao=true/has_acapn=true (one copy, not two).
//   (c) handlers that do NOT call set_header leave has_acao/has_acapn false.
//   (d) the OTA route handler stubs emit CORS headers exactly once.
// ---------------------------------------------------------------------------

void test_capture_cors_headers_recorded(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    // Simulate a handler that explicitly sets CORS headers (e.g. ota_update_handler)
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");
    bb_http_resp_sendstr(req, "{}");

    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_TRUE_MESSAGE(cap.has_acao,  "Access-Control-Allow-Origin not captured");
    TEST_ASSERT_TRUE_MESSAGE(cap.has_acapn, "Access-Control-Allow-Private-Network not captured");

    bb_http_host_capture_free(&cap);
}

void test_capture_cors_absent_by_default(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    // Handler that does NOT set CORS headers — shim no longer injects globally
    bb_http_resp_sendstr(req, "{}");

    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_FALSE_MESSAGE(cap.has_acao,  "has_acao should be false without explicit set_header");
    TEST_ASSERT_FALSE_MESSAGE(cap.has_acapn, "has_acapn should be false without explicit set_header");

    bb_http_host_capture_free(&cap);
}

// OTA update/progress handler stub: mirrors ota_status_handler CORS injection.
static bb_err_t h_ota_progress_cors(bb_http_request_t *req)
{
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "state", "idle");
    bb_http_resp_json_obj_set_bool(&obj, "in_progress", false);
    bb_http_resp_json_obj_set_int(&obj, "progress_pct", 0);
    return bb_http_resp_json_obj_end(&obj);
}

// OTA apply handler stub: mirrors ota_update_handler CORS injection.
static bb_err_t h_ota_apply_cors(bb_http_request_t *req)
{
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");
    bb_http_resp_set_status(req, 409);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "error", "update_in_progress");
    return bb_http_resp_json_obj_end(&obj);
}

// Update-status handler stub: mirrors status_handler CORS injection.
static bb_err_t h_update_status_cors(bb_http_request_t *req)
{
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "current", "v1.0.0");
    bb_http_resp_json_obj_set_str(&obj, "latest", "v1.0.0");
    bb_http_resp_json_obj_set_str(&obj, "download_url", "");
    bb_http_resp_json_obj_set_bool(&obj, "available", false);
    bb_http_resp_json_obj_set_bool(&obj, "last_check_ok", false);
    bb_http_resp_json_obj_set_bool(&obj, "enabled", true);
    bb_http_resp_json_obj_set_str(&obj, "outcome", "unknown");
    return bb_http_resp_json_obj_end(&obj);
}

void test_ota_progress_handler_emits_cors_headers(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    h_ota_progress_cors(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_TRUE_MESSAGE(cap.has_acao,  "GET /api/update/progress: missing ACAO");
    TEST_ASSERT_TRUE_MESSAGE(cap.has_acapn, "GET /api/update/progress: missing ACAPN");
    bb_http_host_capture_free(&cap);
}

void test_ota_apply_handler_emits_cors_headers(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    h_ota_apply_cors(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_TRUE_MESSAGE(cap.has_acao,  "POST /api/update/apply: missing ACAO");
    TEST_ASSERT_TRUE_MESSAGE(cap.has_acapn, "POST /api/update/apply: missing ACAPN");
    bb_http_host_capture_free(&cap);
}

void test_update_status_handler_emits_cors_headers(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    h_update_status_cors(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_TRUE_MESSAGE(cap.has_acao,  "GET /api/update/status: missing ACAO");
    TEST_ASSERT_TRUE_MESSAGE(cap.has_acapn, "GET /api/update/status: missing ACAPN");
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// Capture harness unit tests
// ---------------------------------------------------------------------------

void test_capture_begin_end_basic(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    TEST_ASSERT_NOT_NULL(req);

    bb_http_resp_set_status(req, 201);
    bb_http_resp_set_type(req, "application/json");
    bb_http_resp_sendstr(req, "{\"x\":1}");

    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_err_t rc = bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(201, cap.status);
    TEST_ASSERT_EQUAL_STRING("application/json", cap.content_type);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_EQUAL_STRING("{\"x\":1}", cap.body);
    bb_http_host_capture_free(&cap);
}

void test_capture_default_status_is_200(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_capture_send_json_sets_content_type(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "k", "v");
    bb_http_resp_json_obj_end(&obj);

    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_EQUAL_STRING("application/json", cap.content_type);
    TEST_ASSERT_NOT_NULL(cap.body);
    bb_http_host_capture_free(&cap);
}

void test_capture_multi_write_appends(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    bb_http_resp_send_chunk(req, "aaa", 3);
    bb_http_resp_send_chunk(req, "bbb", 3);
    bb_http_resp_send_chunk(req, NULL, 0);

    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_EQUAL_INT(6, (int)cap.body_len);
    TEST_ASSERT_EQUAL_STRING("aaabbb", cap.body);
    bb_http_host_capture_free(&cap);
}

void test_capture_end_null_args_returns_err(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_host_capture_end(NULL, &cap));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_host_capture_end(req, NULL));

    // clean up slot
    bb_http_host_capture_end(req, &cap);
    bb_http_host_capture_free(&cap);
}

void test_capture_no_active_slot_ignored(void)
{
    // Calls to resp helpers with no active capture should be safe no-ops.
    bb_http_request_t *fake = (bb_http_request_t *)&(int){99};
    bb_http_resp_set_status(fake, 500);
    bb_http_resp_set_type(fake, "text/plain");
    bb_http_resp_send_chunk(fake, "hello", 5);
    bb_http_resp_sendstr(fake, "world");
    // No crash = pass
}

// ---------------------------------------------------------------------------
// /api/health: no raw numeric values anywhere in the response (TA-505).
// ---------------------------------------------------------------------------

static void assert_no_numbers_recursive(cJSON *node, const char *path)
{
    if (!node) return;
    if (cJSON_IsNumber(node)) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "/api/health: numeric value at '%s' (TA-505: health must carry "
                 "no raw numbers; move them to /api/diag/wifi)", path);
        TEST_FAIL_MESSAGE(msg);
    }
    cJSON *child = node->child;
    char child_path[256];
    while (child) {
        if (child->string) {
            snprintf(child_path, sizeof(child_path), "%s.%s", path, child->string);
        } else {
            snprintf(child_path, sizeof(child_path), "%s[]", path);
        }
        assert_no_numbers_recursive(child, child_path);
        child = child->next;
    }
}

void test_fidelity_health_no_raw_numbers(void)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    h_health(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_NOT_NULL_MESSAGE(cap.body, "health body is null");
    cJSON *parsed = cJSON_Parse(cap.body);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "health body is not valid JSON");
    assert_no_numbers_recursive(parsed, "root");
    cJSON_Delete(parsed);
    bb_http_host_capture_free(&cap);
}

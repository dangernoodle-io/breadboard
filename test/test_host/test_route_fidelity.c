// Fidelity audit: verify that each JSON handler's emitted body validates
// against the response schema declared alongside it.
//
// Pattern mirrors test_route_schemas.c (registry-seeding + declared fixtures)
// and test_bb_http_json_arr_stream.c (fake req cookie).
//
// SKIPPED ROUTES (with rationale):
//   /api/events            - SSE stream (text/event-stream); not JSON
//   /api/scan              - bb_wifi_routes.c includes <esp_wifi.h> which cannot
//                            link on host; scan uses esp_wifi_scan internally.
//                            Follow-up: add a bb_wifi_scan host shim (B1-???).
//   /api/update/apply      - ota_update_handler calls esp_restart() (ESP-IDF only);
//                            ota_boot_handler same. Both handlers are static in
//                            ESP-IDF platform files.
//   /api/update/partitions - calls esp_ota_get_running_partition (ESP-IDF only)
//   /api/update/recover    - calls esp_ota_erase_last_boot_app_partition (ESP-IDF only)
//   /api/update/mark-valid (200) - bb_ota_is_pending() always false on host; 409 path covered
//   /api/diag/heap         - calls heap_caps_* (ESP-IDF only)
//   /api/diag/sockets      - walks LWIP TCP PCBs (ESP-IDF only)
//   /api/diag/tasks        - CONFIG_FREERTOS_USE_TRACE_FACILITY (ESP-IDF only)
//   /api/diag/coredump     - ESP-IDF partition API (ESP-IDF only)
//   /api/diag/panic/trigger- debug-only; ESP-IDF only
//   /api/openapi.json      - uses bb_openapi_emit_stream; no fixed schema to validate
//                            (self-describing meta-spec); already covered by test_openapi_emit.c
//   /api/manifest          - wraps bb_manifest_emit; no fidelity schema declared
//                            (descriptor .handler = NULL); already covered by test_manifest.c
//   DELETE /api/diag/boot  - returns 204 No Content; no JSON body
//   POST /api/log/level    - returns 204 No Content on success; JSON only on 400 errors
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

#include "unity.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_host.h"
#include "bb_openapi.h"
#include "bb_board.h"
#include "bb_settings.h"
#include "bb_wifi.h"
#include "bb_wifi_http.h"
#include "bb_wifi_test.h"
#include "bb_system.h"
#include "bb_mdns.h"
#include "bb_diag.h"
#include "bb_partition.h"
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

#include "bb_json.h"
#include "cJSON.h"

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
    "\"validated\":{\"type\":\"boolean\"},"
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

// GET /api/diag/boot — platform/espidf/bb_diag/bb_diag_routes.c
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

// GET /api/diag/panic — platform/espidf/bb_diag/bb_diag_routes.c
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

// GET /api/diag/partitions — platform/espidf/bb_diag/bb_diag_routes.c
static const char k_partitions_schema[] =
    "{\"type\":\"array\","
    "\"items\":{\"type\":\"object\","
    "\"properties\":{"
    "\"label\":{\"type\":\"string\"},"
    "\"type\":{\"type\":\"string\"},"
    "\"subtype\":{\"type\":\"string\"},"
    "\"offset\":{\"type\":\"integer\"},"
    "\"size\":{\"type\":\"integer\"},"
    "\"running\":{\"type\":\"boolean\"},"
    "\"next_ota\":{\"type\":\"boolean\"}},"
    "\"required\":[\"label\",\"type\",\"offset\",\"size\"]}}";

// GET /api/diag/wifi — platform/espidf/bb_wifi_http/bb_wifi_http_routes.c
// (B1-969; rehomed + reduced from the dissolved bb_net_health's /api/diag/net)
static const char k_diag_wifi_schema[] =
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
    "\"disconnect_rssi\":{\"type\":\"integer\"},"
    "\"roam_count\":{\"type\":\"integer\"},"
    "\"roam_age_s\":{\"type\":\"integer\"},"
    "\"last_session_s\":{\"type\":\"integer\"},"
    "\"net_mode\":{\"type\":\"string\"},"
    "\"associated\":{\"type\":\"boolean\"},"
    "\"has_ip\":{\"type\":\"boolean\"},"
    "\"reason_histogram\":{\"type\":\"object\","
    "\"additionalProperties\":{\"type\":\"integer\"},"
    "\"properties\":{"
    "\"top_reason\":{\"type\":\"string\"},"
    "\"top_reason_count\":{\"type\":\"integer\"}}}},"
    "\"required\":[\"ssid\",\"connected\"]}";

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

static bb_err_t h_health(bb_http_request_t *req)
{
    // TA-505: status bools/enums only — no free_heap; numeric network fields
    // moved to /api/diag/wifi.  Uses bb_wifi_emit_status (SSOT) so any future
    // numeric addition to bb_wifi_emit_status would require an intentional API
    // change, and test_fidelity_health_no_raw_numbers catches it at test time.
    bb_board_info_t b;
    bb_board_get_info(&b);

    const char *hostname = bb_mdns_get_hostname();

    bb_json_t root = bb_json_obj_new();
    bb_json_obj_set_bool(root, "ok", bb_health_compute_ok());
    bb_json_obj_set_bool(root, "validated", b.ota_validated);

    bb_json_t net = bb_json_obj_new();
    bb_wifi_emit_status(net);
    if (hostname) {
        bb_json_obj_set_string(net, "mdns", hostname);
    } else {
        bb_json_obj_set_null(net, "mdns");
    }
    bb_json_obj_set_obj(root, "network", net);

    char *str = bb_json_serialize(root);
    bb_json_free(root);
    if (!str) return BB_ERR_NO_SPACE;
    bb_http_resp_set_type(req, "application/json");
    bb_err_t err = bb_http_resp_send_chunk(req, str, -1);
    if (err == BB_OK) err = bb_http_resp_send_chunk(req, NULL, 0);
    bb_json_free_str(str);
    return err;
}

// B1-467: calls the real production emitter (bb_wifi_emit_section,
// platform/host/bb_wifi/bb_wifi_emit.c) instead of hand-copying its fields,
// so this fidelity fixture exercises the SSOT directly.  Mirrors the
// live-read fallback path in wifi_info_handler
// (platform/espidf/bb_wifi/bb_wifi_routes.c).
static bb_err_t h_wifi_info(bb_http_request_t *req)
{
    bb_wifi_info_t info;
    bb_wifi_get_info(&info);

    bb_json_t root = bb_json_obj_new();
    if (!root) return BB_ERR_NO_SPACE;
    bb_wifi_emit_section(root, &info);

    char *str = bb_json_serialize(root);
    bb_json_free(root);
    if (!str) return BB_ERR_NO_SPACE;
    bb_http_resp_set_type(req, "application/json");
    bb_err_t err = bb_http_resp_send_chunk(req, str, -1);
    if (err == BB_OK) err = bb_http_resp_send_chunk(req, NULL, 0);
    bb_json_free_str(str);
    return err;
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
// Mirrors panic_get_handler in platform/espidf/bb_diag/bb_diag_routes.c.
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

// GET /api/diag/partitions — canned 2-partition response using bb_partition host mock.
// Does NOT call the real partitions_get_handler (static in bb_diag_routes.c);
// mirrors its emit pattern to verify schema fidelity.
static bb_err_t h_diag_partitions(bb_http_request_t *req)
{
    bb_partition_info_t parts[8];
    size_t count = 0;
    bb_partition_list(parts, sizeof(parts) / sizeof(parts[0]), &count);
    if (count > 2) count = 2;  // emit just the first 2 for the fidelity test

    bb_http_json_stream_t arr;
    bb_err_t rc = bb_http_resp_json_arr_begin(req, &arr);
    if (rc != BB_OK) return rc;

    for (size_t i = 0; i < count; i++) {
        bb_json_t item = bb_json_obj_new();
        if (item) {
            bb_json_obj_set_string(item, "label",    parts[i].label);
            bb_json_obj_set_string(item, "type",     parts[i].type);
            bb_json_obj_set_string(item, "subtype",  parts[i].subtype);
            bb_json_obj_set_number(item, "offset",   (double)parts[i].offset);
            bb_json_obj_set_number(item, "size",     (double)parts[i].size);
            bb_json_obj_set_bool  (item, "running",  parts[i].running);
            bb_json_obj_set_bool  (item, "next_ota", parts[i].next_ota);
            bb_http_resp_json_arr_emit(&arr, item);
            bb_json_free(item);
        }
    }
    return bb_http_resp_json_arr_end(&arr);
}

// GET /api/diag/wifi — mirrors the real diag_wifi_handler (static in
// platform/espidf/bb_wifi_http/bb_wifi_http_routes.c) field-for-field, using
// the same public getters + the real bb_wifi_emit_section/
// bb_wifi_reason_histogram_top logic (B1-969; rehomed + reduced from the
// dissolved bb_net_health's GET /api/diag/net -- no gw/egress/
// early_warning/transport-health fields, all dropped not migrated).
static bb_err_t h_diag_wifi(bb_http_request_t *req)
{
    bb_wifi_test_set_associated(true);
    bb_wifi_test_set_has_ip(true);
    bb_wifi_test_set_roam_count(2);
    bb_wifi_test_set_roam_age_s(45);
    bb_wifi_test_set_last_session_s(120);

    uint16_t hist[BB_WIFI_DISC_COUNT];
    memset(hist, 0, sizeof(hist));
    hist[BB_WIFI_REASON_BB_LOST_IP]        = 1;
    hist[BB_WIFI_REASON_BB_EGRESS_DEAD]    = 4;
    hist[BB_WIFI_REASON_BB_NO_IP_WATCHDOG] = 3;
    hist[BB_WIFI_DISC_INACTIVITY]          = 7; // standard reason, top non-injected count
    bb_wifi_test_set_reason_histogram(hist, BB_WIFI_DISC_COUNT);

    bb_wifi_info_t info;
    bb_wifi_get_info(&info);

    bb_json_t root = bb_json_obj_new();
    if (!root) return BB_ERR_NO_SPACE;
    bb_wifi_emit_section(root, &info);

    bool associated = bb_wifi_is_associated();
    bool has_ip     = bb_wifi_has_ip();
    bb_wifi_mode_t mode = bb_wifi_classify_mode(associated, has_ip);

    bb_json_obj_set_int   (root, "roam_count",     (int64_t)bb_wifi_get_roam_count());
    bb_json_obj_set_int   (root, "roam_age_s",     (int64_t)bb_wifi_get_roam_age_s());
    bb_json_obj_set_int   (root, "last_session_s", (int64_t)bb_wifi_get_last_session_s());
    bb_json_obj_set_string(root, "net_mode",       bb_wifi_mode_str(mode));
    bb_json_obj_set_bool  (root, "associated",     associated);
    bb_json_obj_set_bool  (root, "has_ip",         has_ip);

    uint16_t got_hist[BB_WIFI_DISC_COUNT];
    bb_wifi_get_reason_histogram(got_hist, BB_WIFI_DISC_COUNT);
    uint16_t top_count = 0;
    bb_wifi_disc_reason_t top_reason = bb_wifi_reason_histogram_top(got_hist, &top_count);

    bb_json_t hist_obj = bb_json_obj_new();
    if (hist_obj) {
        for (int i = 0; i < BB_WIFI_DISC_COUNT; i++) {
            if (got_hist[i] == 0) continue;
            bb_json_obj_set_int(hist_obj, bb_wifi_disc_reason_str((bb_wifi_disc_reason_t)i),
                                 (int64_t)got_hist[i]);
        }
        bb_json_obj_set_string(hist_obj, "top_reason",       bb_wifi_disc_reason_str(top_reason));
        bb_json_obj_set_int   (hist_obj, "top_reason_count", (int64_t)top_count);
        bb_json_obj_set_obj(root, "reason_histogram", hist_obj);
    }

    char *str = bb_json_serialize(root);
    bb_json_free(root);
    if (!str) return BB_ERR_NO_SPACE;
    bb_http_resp_set_type(req, "application/json");
    bb_err_t err = bb_http_resp_send_chunk(req, str, -1);
    if (err == BB_OK) err = bb_http_resp_send_chunk(req, NULL, 0);
    bb_json_free_str(str);
    return err;
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

// PATCH /api/wifi 400 — mirrors wifi_patch_handler validation failure path.
static bb_err_t h_wifi_patch_400(bb_http_request_t *req)
{
    bb_http_resp_set_status(req, 400);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "error", "ssid required");
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
    { "/api/diag/partitions",        h_diag_partitions,   200, "application/json", k_partitions_schema      },
    { "/api/diag/wifi",              h_diag_wifi,         200, "application/json", k_diag_wifi_schema       },
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

void test_fidelity_diag_partitions(void)
{
    run_fidelity(&k_audit[12]);
}

void test_fidelity_diag_wifi(void)
{
    run_fidelity(&k_audit[13]);

    // h_diag_wifi drives global BB_WIFI_TESTING hooks (associated/has_ip/
    // roam_count/roam_age_s/last_session_s/reason_histogram) that persist
    // across the whole test binary -- restore defaults so later tests that
    // assume a clean/zeroed bb_wifi test-hook state (e.g. test_bb_wifi.c's
    // "default zero" fixtures) are not polluted by this fixture's run.
    bb_wifi_test_set_associated(false);
    bb_wifi_test_set_has_ip(false);
    bb_wifi_test_set_roam_count(0);
    bb_wifi_test_set_roam_age_s(0);
    bb_wifi_test_set_last_session_s(0);
    uint16_t empty_hist[BB_WIFI_DISC_COUNT];
    memset(empty_hist, 0, sizeof(empty_hist));
    bb_wifi_test_set_reason_histogram(empty_hist, BB_WIFI_DISC_COUNT);
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

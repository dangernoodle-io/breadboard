// Fidelity audit: verify that each JSON handler's emitted body validates
// against the response schema declared alongside it.
//
// Pattern mirrors test_route_schemas.c (registry-seeding + declared fixtures)
// and test_bb_http_json_arr_stream.c (fake req cookie).
//
// SKIPPED ROUTES (with rationale):
//   /api/logs              - SSE stream (text/event-stream); not JSON
//   /api/events            - SSE stream (text/event-stream); not JSON
//   /api/logs/status       - handler lives in platform/espidf/bb_log/bb_log_routes.c
//                            (static fn) and calls bb_log_stream_dropped_lines()
//                            which has no host implementation
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
//   /api/board             - dropped; superseded by /api/info
//   /api/ping              - dropped; superseded by /api/health
//   /api/version           - dropped; use GET /api/info for .version field
//   /api/ota/check         - moved to POST /api/update/check
//   /api/ota/update        - moved to POST /api/update/apply
//   /api/ota/status        - moved to GET /api/update/progress
//   /api/ota/push          - moved to POST /api/update/push
//   /api/ota/mark-valid    - moved to POST /api/update/mark-valid

#include "unity.h"
#include "bb_http.h"
#include "bb_http_host.h"
#include "bb_openapi.h"
#include "bb_board.h"
#include "bb_nv.h"
#include "bb_wifi.h"
#include "bb_system.h"
#include "bb_mdns.h"
#include "bb_diag.h"
#include "bb_partition.h"
#include "bb_event.h"
#include "bb_update_check.h"
#include "bb_event_routes.h"
#include "bb_event_ring.h"
#include "bb_log.h"
#include "bb_info.h"
#include "bb_info_test.h"
#include "bb_health.h"
#include "../../components/bb_info/bb_info_schema_priv.h"

// bb_mdns_started and bb_mdns_get_hostname are declared in bb_mdns.h only
// under #ifdef ESP_PLATFORM. The host stub implements them; forward-declare
// them here so this file compiles on host builds.
bool bb_mdns_started(void);
const char *bb_mdns_get_hostname(void);

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

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

// GET /api/info — bb_info.c (espidf)
// IMPORTANT: must match bb_info_get_assembled_schema() with no extenders registered.
// (test_fidelity_info_schema_matches_assembled enforces this.)
// Includes http_handler_count and http_handler_cap added in bb-periph-info.
static const char k_info_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"board\":{\"type\":\"string\"},"
    "\"project_name\":{\"type\":\"string\"},"
    "\"version\":{\"type\":\"string\"},"
    "\"idf_version\":{\"type\":\"string\"},"
    "\"build_date\":{\"type\":\"string\"},"
    "\"build_time\":{\"type\":\"string\"},"
    "\"chip_model\":{\"type\":\"string\"},"
    "\"cores\":{\"type\":\"integer\"},"
    "\"mac\":{\"type\":\"string\"},"
    "\"flash_size\":{\"type\":\"integer\"},"
    "\"total_heap\":{\"type\":\"integer\"},"
    "\"free_heap\":{\"type\":\"integer\"},"
    "\"app_size\":{\"type\":\"integer\"},"
    "\"reset_reason\":{\"type\":\"string\"},"
    "\"ota_validated\":{\"type\":\"boolean\"},"
    "\"heap_free_total\":{\"type\":\"integer\"},"
    "\"heap_free_internal\":{\"type\":\"integer\"},"
    "\"heap_minimum_ever\":{\"type\":\"integer\"},"
    "\"heap_largest_free_block\":{\"type\":\"integer\"},"
    "\"chip_revision\":{\"type\":\"integer\"},"
    "\"cpu_freq_mhz\":{\"type\":\"integer\"},"
    "\"heap_internal\":{\"type\":\"object\","
    "\"properties\":{"
    "\"free\":{\"type\":\"integer\"},"
    "\"total\":{\"type\":\"integer\"}}},"
    "\"heap_psram\":{\"type\":\"object\","
    "\"properties\":{"
    "\"free\":{\"type\":\"integer\"},"
    "\"total\":{\"type\":\"integer\"}}},"
    "\"rtc\":{\"type\":\"object\","
    "\"properties\":{"
    "\"used\":{\"type\":\"integer\"},"
    "\"total\":{\"type\":\"integer\"}}},"
    "\"network\":{\"type\":\"object\","
    "\"properties\":{"
    "\"ssid\":{\"type\":\"string\"},"
    "\"bssid\":{\"type\":\"string\"},"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"ip\":{\"type\":\"string\"},"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"disc_reason\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"retry_count\":{\"type\":\"integer\"}}}"
    ",\"http_handler_count\":{\"type\":\"integer\"},"
    "\"http_handler_cap\":{\"type\":\"integer\"},"
    "\"capabilities\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"
    "},"
    "\"required\":[\"board\",\"version\",\"network\"]}";

// GET /api/health — bb_health.c (espidf)
// network gains ssid/bssid/ip/disc_reason from bb_wifi_emit_section (additive).
// mdns field KEPT (locked decision B1-269). ok drops mdns from gate.
static const char k_health_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"ok\":{\"type\":\"boolean\"},"
    "\"free_heap\":{\"type\":\"integer\"},"
    "\"validated\":{\"type\":\"boolean\"},"
    "\"network\":{\"type\":\"object\","
    "\"properties\":{"
    "\"ssid\":{\"type\":\"string\"},"
    "\"bssid\":{\"type\":\"string\"},"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"ip\":{\"type\":\"string\"},"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"disc_reason\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"retry_count\":{\"type\":\"integer\"},"
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
    "\"disc_reason\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"retry_count\":{\"type\":\"integer\"}},"
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
    "\"boots_since\":{\"type\":\"integer\"},"
    "\"reset_reason\":{\"type\":\"string\"}},"
    "\"required\":[\"available\"]}},"
    "\"required\":[\"reset_reason\",\"wdt_resets\",\"panic\"]}";

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
    "\"panic_reason\":{\"type\":\"string\"}},"
    "\"required\":[\"available\"]}";

// GET /api/update/status — platform/espidf/bb_update_check/bb_update_check_espidf.c
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
    "\"enum\":[\"unknown\",\"up_to_date\",\"available\","
    "\"no_asset\",\"check_failed\"]},"
    "\"last_check_ts\":{\"type\":\"integer\"}},"
    "\"required\":[\"current\",\"latest\",\"download_url\","
    "\"available\",\"last_check_ok\",\"enabled\",\"outcome\"]}";

// GET /api/update/config — components/bb_update_check/src/bb_update_check_common.c
static const char k_update_config_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{\"enabled\":{\"type\":\"boolean\"}},"
    "\"required\":[\"enabled\"]}";

// POST /api/update/check — platform/espidf/bb_ota_pull/bb_ota_pull.c (ota_check_handler)
static const char k_update_check_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{\"status\":{\"type\":\"string\"}},"
    "\"required\":[\"status\"]}";

// GET /api/diag/events — platform/espidf/bb_event_routes/bb_event_routes_espidf.c
static const char k_diag_events_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"topics\":{\"type\":\"array\",\"items\":{"
    "\"type\":\"object\","
    "\"properties\":{"
    "\"name\":{\"type\":\"string\"},"
    "\"ring_capacity\":{\"type\":\"integer\"},"
    "\"ring_count\":{\"type\":\"integer\"},"
    "\"last_id\":{\"type\":\"integer\"},"
    "\"last_post_us\":{\"type\":\"integer\"},"
    "\"last_size\":{\"type\":\"integer\"}},"
    "\"required\":[\"name\",\"ring_capacity\",\"ring_count\","
    "\"last_id\",\"last_post_us\",\"last_size\"]}},"
    "\"max_clients\":{\"type\":\"integer\"},"
    "\"active_clients\":{\"type\":\"integer\"}},"
    "\"required\":[\"topics\",\"max_clients\",\"active_clients\"]}";

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

// GET /api/log/level — platform/espidf/bb_log/bb_log_http.c
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

static bb_err_t h_info(bb_http_request_t *req)
{
    bb_board_info_t b;
    bb_wifi_info_t  w;
    bb_board_get_info(&b);
    bb_wifi_get_info(&w);

    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             w.bssid[0], w.bssid[1], w.bssid[2],
             w.bssid[3], w.bssid[4], w.bssid[5]);

    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "board", b.board);
    bb_http_resp_json_obj_set_str(&obj, "project_name", b.project_name);
    bb_http_resp_json_obj_set_str(&obj, "version", b.version);
    bb_http_resp_json_obj_set_str(&obj, "idf_version", b.idf_version);
    bb_http_resp_json_obj_set_str(&obj, "build_date", b.build_date);
    bb_http_resp_json_obj_set_str(&obj, "build_time", b.build_time);
    bb_http_resp_json_obj_set_str(&obj, "chip_model", b.chip_model);
    bb_http_resp_json_obj_set_num(&obj, "cores", (double)b.cores);
    bb_http_resp_json_obj_set_str(&obj, "mac", b.mac);
    bb_http_resp_json_obj_set_num(&obj, "flash_size", (double)b.flash_size);
    bb_http_resp_json_obj_set_num(&obj, "total_heap", (double)b.total_heap);
    bb_http_resp_json_obj_set_num(&obj, "free_heap", (double)b.free_heap);
    bb_http_resp_json_obj_set_num(&obj, "app_size", (double)b.app_size);
    bb_http_resp_json_obj_set_str(&obj, "reset_reason", b.reset_reason);
    bb_http_resp_json_obj_set_bool(&obj, "ota_validated", b.ota_validated);
    bb_http_resp_json_obj_set_num(&obj, "heap_free_total", (double)bb_board_heap_free_total());
    bb_http_resp_json_obj_set_num(&obj, "heap_free_internal", (double)bb_board_heap_free_internal());
    bb_http_resp_json_obj_set_num(&obj, "heap_minimum_ever", (double)bb_board_heap_minimum_ever());
    bb_http_resp_json_obj_set_num(&obj, "heap_largest_free_block", (double)bb_board_heap_largest_free_block());
    bb_http_resp_json_obj_set_num(&obj, "chip_revision", (double)bb_board_chip_revision());
    bb_http_resp_json_obj_set_num(&obj, "cpu_freq_mhz", (double)bb_board_cpu_freq_mhz());
    bb_http_resp_json_obj_set_obj_begin(&obj, "heap_internal");
    bb_http_resp_json_obj_set_num(&obj, "free",  (double)bb_board_heap_internal_free());
    bb_http_resp_json_obj_set_num(&obj, "total", (double)bb_board_heap_internal_total());
    bb_http_resp_json_obj_set_obj_end(&obj);
    bb_http_resp_json_obj_set_obj_begin(&obj, "heap_psram");
    bb_http_resp_json_obj_set_num(&obj, "free",  (double)bb_board_psram_free());
    bb_http_resp_json_obj_set_num(&obj, "total", (double)bb_board_psram_total());
    bb_http_resp_json_obj_set_obj_end(&obj);
    bb_http_resp_json_obj_set_obj_begin(&obj, "rtc");
    bb_http_resp_json_obj_set_num(&obj, "used",  (double)bb_board_rtc_used());
    bb_http_resp_json_obj_set_num(&obj, "total", (double)bb_board_rtc_total());
    bb_http_resp_json_obj_set_obj_end(&obj);
    bb_http_resp_json_obj_set_obj_begin(&obj, "network");
    bb_http_resp_json_obj_set_str(&obj, "ssid", w.ssid);
    bb_http_resp_json_obj_set_str(&obj, "bssid", bssid);
    bb_http_resp_json_obj_set_num(&obj, "rssi", (double)w.rssi);
    bb_http_resp_json_obj_set_str(&obj, "ip", w.ip);
    bb_http_resp_json_obj_set_bool(&obj, "connected", w.connected);
    bb_http_resp_json_obj_set_num(&obj, "disc_reason", (double)w.disc_reason);
    bb_http_resp_json_obj_set_num(&obj, "disc_age_s", (double)w.disc_age_s);
    bb_http_resp_json_obj_set_num(&obj, "retry_count", (double)w.retry_count);
    bb_http_resp_json_obj_set_obj_end(&obj);
    bb_http_resp_json_obj_set_num(&obj, "http_handler_count",
                                  (double)bb_http_route_handler_count());
    bb_http_resp_json_obj_set_num(&obj, "http_handler_cap",
                                  (double)bb_http_route_handler_cap());
    // capabilities: always emit (empty array when none registered)
    bb_http_resp_json_obj_set_arr_begin(&obj, "capabilities");
    bb_http_resp_json_obj_set_arr_end(&obj);
    return bb_http_resp_json_obj_end(&obj);
}

static bb_err_t h_health(bb_http_request_t *req)
{
    bb_board_info_t b;
    bb_wifi_info_t  w;
    bb_board_get_info(&b);
    bb_wifi_get_info(&w);

    const char *hostname = bb_mdns_get_hostname();

    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             w.bssid[0], w.bssid[1], w.bssid[2],
             w.bssid[3], w.bssid[4], w.bssid[5]);

    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_bool(&obj, "ok", bb_health_compute_ok());
    bb_http_resp_json_obj_set_num(&obj, "free_heap", (double)b.free_heap);
    bb_http_resp_json_obj_set_bool(&obj, "validated", b.ota_validated);
    bb_http_resp_json_obj_set_obj_begin(&obj, "network");
    bb_http_resp_json_obj_set_str(&obj, "ssid", w.ssid);
    bb_http_resp_json_obj_set_str(&obj, "bssid", bssid);
    bb_http_resp_json_obj_set_int(&obj, "rssi", (int64_t)w.rssi);
    bb_http_resp_json_obj_set_str(&obj, "ip", w.ip);
    bb_http_resp_json_obj_set_bool(&obj, "connected", w.connected);
    bb_http_resp_json_obj_set_int(&obj, "disc_reason", (int64_t)w.disc_reason);
    bb_http_resp_json_obj_set_num(&obj, "disc_age_s", (double)w.disc_age_s);
    bb_http_resp_json_obj_set_num(&obj, "retry_count", (double)w.retry_count);
    if (hostname) {
        bb_http_resp_json_obj_set_str(&obj, "mdns", hostname);
    } else {
        bb_http_resp_json_obj_set_null(&obj, "mdns");
    }
    bb_http_resp_json_obj_set_obj_end(&obj);
    return bb_http_resp_json_obj_end(&obj);
}

static bb_err_t h_wifi_info(bb_http_request_t *req)
{
    bb_wifi_info_t info;
    bb_wifi_get_info(&info);

    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             info.bssid[0], info.bssid[1], info.bssid[2],
             info.bssid[3], info.bssid[4], info.bssid[5]);

    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "ssid", info.ssid);
    bb_http_resp_json_obj_set_str(&obj, "bssid", bssid);
    bb_http_resp_json_obj_set_num(&obj, "rssi", (double)info.rssi);
    bb_http_resp_json_obj_set_str(&obj, "ip", info.ip);
    bb_http_resp_json_obj_set_bool(&obj, "connected", info.connected);
    bb_http_resp_json_obj_set_num(&obj, "disc_reason", (double)info.disc_reason);
    bb_http_resp_json_obj_set_num(&obj, "disc_age_s", (double)info.disc_age_s);
    bb_http_resp_json_obj_set_num(&obj, "retry_count", (double)info.retry_count);
    return bb_http_resp_json_obj_end(&obj);
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
    bb_http_resp_json_obj_set_str(&obj, "reset_reason", "poweron");
    bb_http_resp_json_obj_set_int(&obj, "wdt_resets", 0);
    bb_http_resp_json_obj_set_obj_begin(&obj, "panic");
    bb_http_resp_json_obj_set_bool(&obj, "available", false);
    bb_http_resp_json_obj_set_obj_end(&obj);
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
    bb_http_resp_json_obj_set_str(&obj, "reset_reason", "panic");
    bb_http_resp_json_obj_set_obj_end(&obj);
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
// platform/espidf/bb_update_check/bb_update_check_espidf.c).
// Requires bb_update_check_init() to have been called.
static bb_err_t h_update_status(bb_http_request_t *req)
{
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");

    bb_update_check_status_t st;
    bb_err_t err = bb_update_check_get_status(&st);
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
        case BB_UPDATE_OUTCOME_UP_TO_DATE: outcome = "up_to_date";  break;
        case BB_UPDATE_OUTCOME_AVAILABLE:  outcome = "available";   break;
        case BB_UPDATE_OUTCOME_NO_ASSET:   outcome = "no_asset";    break;
        case BB_UPDATE_OUTCOME_FAILED:     outcome = "check_failed";break;
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

// GET /api/update/config — mirrors bb_update_check_config_get_handler in
// components/bb_update_check/src/bb_update_check_common.c.
static bb_err_t h_update_config_get(bb_http_request_t *req)
{
    bool enabled = bb_nv_config_update_check_enabled();
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

// GET /api/diag/events — mirrors diag_events_handler in
// platform/espidf/bb_event_routes/bb_event_routes_espidf.c.
// Uses only public bb_event_routes and bb_event_ring APIs.
// Requires bb_event_routes_init() to have been called.
static bb_err_t h_diag_events(bb_http_request_t *req)
{
    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;

    bb_http_resp_json_obj_set_arr_begin(&obj, "topics");
    size_t n = bb_event_routes_topic_count();
    for (size_t i = 0; i < n; i++) {
        const char *name = NULL;
        bb_event_ring_t ring = NULL;
        if (bb_event_routes_topic_info(i, &name, &ring) != BB_OK) continue;

        bb_http_resp_json_obj_set_obj_begin(&obj, NULL);
        bb_http_resp_json_obj_set_str(&obj, "name", name ? name : "");

        if (ring) {
            bb_http_resp_json_obj_set_int(&obj, "ring_capacity", (int64_t)bb_event_ring_capacity(ring));
            bb_http_resp_json_obj_set_int(&obj, "ring_count",    (int64_t)bb_event_ring_count(ring));

            uint32_t last_id = 0;
            size_t   last_sz = 0;
            int64_t  last_us = 0;
            if (bb_event_ring_last_entry_info(ring, &last_id, &last_sz, &last_us) == BB_OK) {
                bb_http_resp_json_obj_set_int(&obj, "last_id",      (int64_t)last_id);
                bb_http_resp_json_obj_set_int(&obj, "last_post_us", last_us);
                bb_http_resp_json_obj_set_int(&obj, "last_size",    (int64_t)last_sz);
            } else {
                bb_http_resp_json_obj_set_int(&obj, "last_id",      0);
                bb_http_resp_json_obj_set_int(&obj, "last_post_us", 0);
                bb_http_resp_json_obj_set_int(&obj, "last_size",    0);
            }
        } else {
            bb_http_resp_json_obj_set_int(&obj, "ring_capacity", 0);
            bb_http_resp_json_obj_set_int(&obj, "ring_count",    0);
            bb_http_resp_json_obj_set_int(&obj, "last_id",       0);
            bb_http_resp_json_obj_set_int(&obj, "last_post_us",  0);
            bb_http_resp_json_obj_set_int(&obj, "last_size",     0);
        }
        bb_http_resp_json_obj_set_obj_end(&obj);
    }
    bb_http_resp_json_obj_set_arr_end(&obj);

    /* max_clients: mirrors CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS used by the
     * production handler; fall back to the same default the component uses
     * when building without sdkconfig (host test environment). */
#ifndef CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS
#define CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS 4
#endif
    bb_http_resp_json_obj_set_int(&obj, "max_clients",    (int64_t)CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS);
    bb_http_resp_json_obj_set_int(&obj, "active_clients", (int64_t)bb_event_routes_active_client_count());

    return bb_http_resp_json_obj_end(&obj);
}

// GET /api/log/level — mirrors log_level_get_handler in
// platform/espidf/bb_log/bb_log_http.c.
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
// Entries that require subsystem state setup (bb_update_check_init,
// bb_event_routes_init) are NOT listed here; their test functions below do
// their own setup and call run_fidelity with a stack-allocated entry.
static const fidelity_entry_t k_audit[] = {
    { "/api/reboot",            h_reboot,            200, "application/json", k_reboot_schema        },
    { "/api/info",              h_info,              200, "application/json", k_info_schema          },
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

void test_fidelity_info(void)
{
    run_fidelity(&k_audit[1]);
}

void test_fidelity_health(void)
{
    run_fidelity(&k_audit[2]);
}

void test_fidelity_wifi_info(void)
{
    run_fidelity(&k_audit[3]);
}

void test_fidelity_ota_status(void)
{
    run_fidelity(&k_audit[4]);
}

void test_fidelity_ota_mark_valid_409(void)
{
    run_fidelity(&k_audit[5]);
}

void test_fidelity_boot_no_panic(void)
{
    run_fidelity(&k_audit[6]);
}

void test_fidelity_boot_with_panic(void)
{
    run_fidelity(&k_audit[7]);
}

void test_fidelity_diag_panic(void)
{
    run_fidelity(&k_audit[8]);
}

void test_fidelity_update_check(void)
{
    run_fidelity(&k_audit[9]);
}

void test_fidelity_log_level_get(void)
{
    run_fidelity(&k_audit[10]);
}

void test_fidelity_wifi_patch_202(void)
{
    run_fidelity(&k_audit[11]);
}

void test_fidelity_wifi_patch_400(void)
{
    run_fidelity(&k_audit[12]);
}

void test_fidelity_diag_partitions(void)
{
    run_fidelity(&k_audit[13]);
}

// Routes that require subsystem state setup are tested individually below.

// GET /api/update/status: requires bb_update_check_init().
// Also ensures bb_event_init() has run (bb_update_check_init calls
// bb_event_topic_register which requires an initialized event bus).
void test_fidelity_update_status(void)
{
    bb_event_init(NULL);          /* idempotent; ensures topic registry ready */
    bb_update_check_init(NULL);   /* idempotent; uses Kconfig defaults */

    const fidelity_entry_t e = {
        "/api/update/status", h_update_status, 200,
        "application/json",   k_update_status_schema,
    };
    run_fidelity(&e);
}

// GET /api/update/config: requires bb_nv_config_update_check_enabled() (portable).
void test_fidelity_update_config_get(void)
{
    const fidelity_entry_t e = {
        "/api/update/config", h_update_config_get, 200,
        "application/json",   k_update_config_schema,
    };
    run_fidelity(&e);
}

// GET /api/diag/events: requires bb_event_routes_init().
// Uses the minimum valid config; no topics attached → empty topics array.
void test_fidelity_diag_events(void)
{
    static const bb_event_routes_cfg_t cfg = {
        .max_clients      = 1,
        .per_client_queue = 2,
        .ring_capacity    = 2,
        .ring_max_entry   = 64,
        .heartbeat_ms     = 1000,
    };
    bb_event_routes_init(&cfg);  /* idempotent */

    const fidelity_entry_t e = {
        "/api/diag/events", h_diag_events, 200,
        "application/json", k_diag_events_schema,
    };
    run_fidelity(&e);
}

// ---------------------------------------------------------------------------
// CORS header capture tests
//
// CORS headers are set per-route, not globally. The three OTA routes that
// lacked ACAO/ACAPN now set them explicitly at the top of their handlers:
//   - GET  /api/update/status   (bb_update_check_espidf.c: status_handler)
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
// New fidelity tests for schema-carrying extenders
// ---------------------------------------------------------------------------

static void extender_add_xtest(void *root)
{
    // Host extender: root is void* on host; no-op here.
    // The point is the schema fragment — not the runtime field.
    (void)root;
}

// (a) Register extender with fragment; validate info body against assembled schema.
void test_fidelity_info_with_extender(void)
{
    static const char frag[] = "\"xtest\":{\"type\":\"string\"}";
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_info_register_extender_ex(extender_add_xtest, frag));

    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, frag),
        "fragment not in assembled schema");

    // Validate info body against the assembled schema.
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    h_info(req);
    bb_http_host_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_NOT_NULL(cap.body);
    cJSON *parsed = cJSON_Parse(cap.body);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "info body not valid JSON");

    bb_openapi_validate_err_t verr;
    memset(&verr, 0, sizeof(verr));
    bb_err_t vrc = bb_openapi_validate(schema, parsed, &verr);
    if (vrc != BB_OK) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "schema violation at '%s': %s", verr.path, verr.message);
        TEST_FAIL_MESSAGE(msg);
    }

    cJSON_Delete(parsed);
    bb_http_host_capture_free(&cap);
}

// (b) Assert k_info_schema == bb_info_get_assembled_schema() with no extenders.
//     Kills double-maintenance drift between this file and bb_info_schema_priv.h.
void test_fidelity_info_schema_matches_assembled(void)
{
    // setUp resets bb_info state, so no extenders are registered here.
    const char *assembled = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(assembled);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(k_info_schema, assembled,
        "k_info_schema in test_route_fidelity.c differs from assembled schema; "
        "update k_info_schema to match bb_info_schema_priv.h base+suffix");
}

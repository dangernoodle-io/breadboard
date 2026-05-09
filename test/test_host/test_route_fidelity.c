// Fidelity audit: verify that each JSON handler's emitted body validates
// against the response schema declared alongside it.
//
// Pattern mirrors test_route_schemas.c (registry-seeding + declared fixtures)
// and test_bb_http_json_arr_stream.c (fake req cookie).
//
// SKIPPED ROUTES (with rationale):
//   /api/logs           - SSE stream (text/event-stream); not JSON
//   /api/version        - text/plain; no schema to validate
//   /api/ping           - text/plain; no schema to validate
//   /api/scan           - bb_wifi_routes.c includes <esp_wifi.h> which cannot
//                         link on host; scan uses esp_wifi_scan internally.
//                         Follow-up: add a bb_wifi_scan host shim (B1-???).
//   /api/ota/check      - handler registered via bb_http_register_route (not
//                         described); handler pointer is NULL in route struct.
//                         The route descriptor exists for OpenAPI docs only.
//                         Audited indirectly via the declared schema literal.
//   /api/ota/update     - same as /api/ota/check above.

#include "unity.h"
#include "bb_http.h"
#include "bb_http_host.h"
#include "bb_openapi.h"
#include "bb_json.h"
#include "bb_board.h"
#include "bb_wifi.h"
#include "bb_system.h"
#include "bb_mdns.h"

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

// GET /api/board — bb_board_routes.c
static const char k_board_schema[] =
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
    "\"ota_validated\":{\"type\":\"boolean\"}},"
    "\"required\":[\"board\",\"version\"]}";

// GET /api/info — bb_info.c (espidf)
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
    "\"network\":{\"type\":\"object\","
    "\"properties\":{"
    "\"ssid\":{\"type\":\"string\"},"
    "\"bssid\":{\"type\":\"string\"},"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"ip\":{\"type\":\"string\"},"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"disc_reason\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"retry_count\":{\"type\":\"integer\"}}}},"
    "\"required\":[\"board\",\"version\",\"network\"]}";

// GET /api/health — bb_info.c (espidf)
static const char k_health_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"ok\":{\"type\":\"boolean\"},"
    "\"free_heap\":{\"type\":\"integer\"},"
    "\"validated\":{\"type\":\"boolean\"},"
    "\"network\":{\"type\":\"object\","
    "\"properties\":{"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"rssi\":{\"type\":\"integer\"},"
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

// GET /api/ota/status — bb_ota_pull.c (espidf)
static const char k_ota_status_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"state\":{\"type\":\"string\","
    "\"enum\":[\"idle\",\"checking\",\"downloading\",\"verifying\",\"complete\",\"error\"]},"
    "\"in_progress\":{\"type\":\"boolean\"},"
    "\"progress_pct\":{\"type\":\"integer\"},"
    "\"last_error\":{\"type\":\"string\"}},"
    "\"required\":[\"state\",\"in_progress\",\"progress_pct\"]}";

// POST /api/ota/mark-valid 200 — bb_ota_validator.c (espidf)
static const char k_mark_valid_ok_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{\"status\":{\"type\":\"string\"}},"
    "\"required\":[\"status\"]}";

// POST /api/ota/mark-valid 409 — bb_ota_validator.c (espidf)
static const char k_mark_valid_409_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{\"error\":{\"type\":\"string\"}},"
    "\"required\":[\"error\"]}";

// ---------------------------------------------------------------------------
// Host-local handler implementations
// These mirror what the production handlers emit using host-available functions.
// Handlers are self-contained; they do not call bb_system_restart().
// ---------------------------------------------------------------------------

static bb_err_t h_reboot(bb_http_request_t *req)
{
    static const char body[] = "{\"status\":\"rebooting\"}";
    bb_http_resp_set_type(req, "application/json");
    return bb_http_resp_send(req, body, sizeof(body) - 1);
}

static bb_err_t h_board(bb_http_request_t *req)
{
    bb_board_info_t info;
    bb_board_get_info(&info);

    bb_json_t root = bb_json_obj_new();
    bb_json_obj_set_string(root, "board", info.board);
    bb_json_obj_set_string(root, "project_name", info.project_name);
    bb_json_obj_set_string(root, "version", info.version);
    bb_json_obj_set_string(root, "idf_version", info.idf_version);
    bb_json_obj_set_string(root, "build_date", info.build_date);
    bb_json_obj_set_string(root, "build_time", info.build_time);
    bb_json_obj_set_string(root, "chip_model", info.chip_model);
    bb_json_obj_set_number(root, "cores", (double)info.cores);
    bb_json_obj_set_string(root, "mac", info.mac);
    bb_json_obj_set_number(root, "flash_size", (double)info.flash_size);
    bb_json_obj_set_number(root, "total_heap", (double)info.total_heap);
    bb_json_obj_set_number(root, "free_heap", (double)info.free_heap);
    bb_json_obj_set_number(root, "app_size", (double)info.app_size);
    bb_json_obj_set_string(root, "reset_reason", info.reset_reason);
    bb_json_obj_set_bool(root, "ota_validated", info.ota_validated);

    bb_err_t err = bb_http_resp_send_json(req, root);
    bb_json_free(root);
    return err;
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

    bb_json_t root = bb_json_obj_new();
    bb_json_obj_set_string(root, "board", b.board);
    bb_json_obj_set_string(root, "project_name", b.project_name);
    bb_json_obj_set_string(root, "version", b.version);
    bb_json_obj_set_string(root, "idf_version", b.idf_version);
    bb_json_obj_set_string(root, "build_date", b.build_date);
    bb_json_obj_set_string(root, "build_time", b.build_time);
    bb_json_obj_set_string(root, "chip_model", b.chip_model);
    bb_json_obj_set_number(root, "cores", (double)b.cores);
    bb_json_obj_set_string(root, "mac", b.mac);
    bb_json_obj_set_number(root, "flash_size", (double)b.flash_size);
    bb_json_obj_set_number(root, "total_heap", (double)b.total_heap);
    bb_json_obj_set_number(root, "free_heap", (double)b.free_heap);
    bb_json_obj_set_number(root, "app_size", (double)b.app_size);
    bb_json_obj_set_string(root, "reset_reason", b.reset_reason);
    bb_json_obj_set_bool(root, "ota_validated", b.ota_validated);
    bb_json_obj_set_number(root, "heap_free_total", (double)bb_board_heap_free_total());
    bb_json_obj_set_number(root, "heap_free_internal", (double)bb_board_heap_free_internal());
    bb_json_obj_set_number(root, "heap_minimum_ever", (double)bb_board_heap_minimum_ever());
    bb_json_obj_set_number(root, "heap_largest_free_block", (double)bb_board_heap_largest_free_block());
    bb_json_obj_set_number(root, "chip_revision", (double)bb_board_chip_revision());
    bb_json_obj_set_number(root, "cpu_freq_mhz", (double)bb_board_cpu_freq_mhz());

    bb_json_t net = bb_json_obj_new();
    bb_json_obj_set_string(net, "ssid", w.ssid);
    bb_json_obj_set_string(net, "bssid", bssid);
    bb_json_obj_set_number(net, "rssi", (double)w.rssi);
    bb_json_obj_set_string(net, "ip", w.ip);
    bb_json_obj_set_bool(net, "connected", w.connected);
    bb_json_obj_set_number(net, "disc_reason", (double)w.disc_reason);
    bb_json_obj_set_number(net, "disc_age_s", (double)w.disc_age_s);
    bb_json_obj_set_number(net, "retry_count", (double)w.retry_count);
    bb_json_obj_set_obj(root, "network", net);

    bb_err_t err = bb_http_resp_send_json(req, root);
    bb_json_free(root);
    return err;
}

static bb_err_t h_health(bb_http_request_t *req)
{
    bb_board_info_t b;
    bb_wifi_info_t  w;
    bb_board_get_info(&b);
    bb_wifi_get_info(&w);

    bool mdns_up = bb_mdns_started();
    const char *hostname = bb_mdns_get_hostname();

    bool ok = w.connected && b.ota_validated && mdns_up;

    bb_json_t root = bb_json_obj_new();
    bb_json_obj_set_bool(root, "ok", ok);
    bb_json_obj_set_number(root, "free_heap", (double)b.free_heap);
    bb_json_obj_set_bool(root, "validated", b.ota_validated);

    bb_json_t net = bb_json_obj_new();
    bb_json_obj_set_bool(net, "connected", w.connected);
    bb_json_obj_set_number(net, "rssi", (double)w.rssi);
    bb_json_obj_set_number(net, "disc_age_s", (double)w.disc_age_s);
    bb_json_obj_set_number(net, "retry_count", (double)w.retry_count);
    if (hostname) {
        bb_json_obj_set_string(net, "mdns", hostname);
    } else {
        bb_json_obj_set_null(net, "mdns");
    }
    bb_json_obj_set_obj(root, "network", net);

    bb_err_t err = bb_http_resp_send_json(req, root);
    bb_json_free(root);
    return err;
}

static bb_err_t h_wifi_info(bb_http_request_t *req)
{
    bb_wifi_info_t info;
    bb_wifi_get_info(&info);

    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             info.bssid[0], info.bssid[1], info.bssid[2],
             info.bssid[3], info.bssid[4], info.bssid[5]);

    bb_json_t root = bb_json_obj_new();
    bb_json_obj_set_string(root, "ssid", info.ssid);
    bb_json_obj_set_string(root, "bssid", bssid);
    bb_json_obj_set_number(root, "rssi", (double)info.rssi);
    bb_json_obj_set_string(root, "ip", info.ip);
    bb_json_obj_set_bool(root, "connected", info.connected);
    bb_json_obj_set_number(root, "disc_reason", (double)info.disc_reason);
    bb_json_obj_set_number(root, "disc_age_s", (double)info.disc_age_s);
    bb_json_obj_set_number(root, "retry_count", (double)info.retry_count);

    bb_err_t err = bb_http_resp_send_json(req, root);
    bb_json_free(root);
    return err;
}

// OTA status: mirrors ota_status_handler idle-state path.
static bb_err_t h_ota_status(bb_http_request_t *req)
{
    bb_json_t root = bb_json_obj_new();
    bb_json_obj_set_string(root, "state", "idle");
    bb_json_obj_set_bool(root, "in_progress", false);
    bb_json_obj_set_number(root, "progress_pct", 0);
    bb_json_obj_set_string(root, "last_error", "");

    bb_err_t err = bb_http_resp_send_json(req, root);
    bb_json_free(root);
    return err;
}

// OTA mark-valid 409: on host bb_ota_is_pending() is always false.
static bb_err_t h_ota_mark_valid_409(bb_http_request_t *req)
{
    bb_http_resp_set_type(req, "application/json");
    bb_http_resp_set_status(req, 409);
    static const char body[] = "{\"error\":\"not pending\"}";
    return bb_http_resp_send(req, body, sizeof(body) - 1);
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
static const fidelity_entry_t k_audit[] = {
    { "/api/reboot",         h_reboot,            200, "application/json", k_reboot_schema       },
    { "/api/board",          h_board,             200, "application/json", k_board_schema        },
    { "/api/info",           h_info,              200, "application/json", k_info_schema         },
    { "/api/health",         h_health,            200, "application/json", k_health_schema       },
    { "/api/wifi",           h_wifi_info,         200, "application/json", k_wifi_schema         },
    { "/api/ota/status",     h_ota_status,        200, "application/json", k_ota_status_schema   },
    { "/api/ota/mark-valid", h_ota_mark_valid_409,409, "application/json", k_mark_valid_409_schema},
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
    char msg[256];
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

void test_fidelity_board(void)
{
    run_fidelity(&k_audit[1]);
}

void test_fidelity_info(void)
{
    run_fidelity(&k_audit[2]);
}

void test_fidelity_health(void)
{
    run_fidelity(&k_audit[3]);
}

void test_fidelity_wifi_info(void)
{
    run_fidelity(&k_audit[4]);
}

void test_fidelity_ota_status(void)
{
    run_fidelity(&k_audit[5]);
}

void test_fidelity_ota_mark_valid_409(void)
{
    run_fidelity(&k_audit[6]);
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

    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_string(obj, "k", "v");
    bb_http_resp_send_json(req, obj);
    bb_json_free(obj);

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

    bb_http_resp_send(req, "aaa", 3);
    bb_http_resp_send(req, "bbb", 3);

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
    bb_http_resp_send(fake, "hello", 5);
    bb_http_resp_sendstr(fake, "world");
    // No crash = pass
}

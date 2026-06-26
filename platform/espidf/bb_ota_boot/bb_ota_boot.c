#include "bb_ota_boot.h"
#include "bb_tls.h"
#include "bb_ota_hooks.h"
#include "bb_log.h"
#include "bb_nv.h"

#include <string.h>

// Flag + breadcrumb live in the bb_cfg namespace (generic bb_nv API). The flag
// is a one-shot mechanism marker, NOT a user config key — deliberately not in
// the bb_nv_config manifest table.
#define OTA_BOOT_NS         "bb_cfg"
#define OTA_BOOT_FLAG_KEY   "ota_boot_mode"
#define OTA_BOOT_STAGE_KEY  "ota_boot_stg"

// Maximum string lengths for set_mdns_service args (hostname/service/proto).
#define OTA_BOOT_MDNS_STR_MAX  64

// mDNS service identity (set by consumer before run_if_pending; used under gated path).
static char     s_mdns_hostname[OTA_BOOT_MDNS_STR_MAX] = {0};
static char     s_mdns_service_type[OTA_BOOT_MDNS_STR_MAX] = {0};
static char     s_mdns_proto[OTA_BOOT_MDNS_STR_MAX] = {0};
static uint16_t s_mdns_port = 0;

// Pure helper: maps OTA phase to the JSON state string for /api/update/progress.
const char *bb_ota_boot_phase_str(bb_ota_phase_t phase)
{
    switch (phase) {
    case BB_OTA_PHASE_START:    return "downloading";
    case BB_OTA_PHASE_PROGRESS: return "downloading";
    case BB_OTA_PHASE_SUCCESS:  return "complete";
    case BB_OTA_PHASE_FAIL:     return "error";
    default:                    return "error";
    }
}

bb_err_t bb_ota_boot_set_mdns_service(const char *hostname,
                                      const char *service_type,
                                      const char *proto,
                                      uint16_t    port)
{
    if (!hostname || !service_type || !proto) {
        return BB_ERR_INVALID_ARG;
    }
    if (strlen(hostname) >= OTA_BOOT_MDNS_STR_MAX ||
        strlen(service_type) >= OTA_BOOT_MDNS_STR_MAX ||
        strlen(proto) >= OTA_BOOT_MDNS_STR_MAX) {
        return BB_ERR_INVALID_ARG;
    }
    strncpy(s_mdns_hostname, hostname, OTA_BOOT_MDNS_STR_MAX - 1);
    s_mdns_hostname[OTA_BOOT_MDNS_STR_MAX - 1] = '\0';
    strncpy(s_mdns_service_type, service_type, OTA_BOOT_MDNS_STR_MAX - 1);
    s_mdns_service_type[OTA_BOOT_MDNS_STR_MAX - 1] = '\0';
    strncpy(s_mdns_proto, proto, OTA_BOOT_MDNS_STR_MAX - 1);
    s_mdns_proto[OTA_BOOT_MDNS_STR_MAX - 1] = '\0';
    s_mdns_port = port;
    return BB_OK;
}

void bb_ota_boot_arm(void)
{
    bb_nv_set_u8(OTA_BOOT_NS, OTA_BOOT_FLAG_KEY, 1);
}

bool bb_ota_boot_pending(void)
{
    uint8_t v = 0;
    bb_nv_get_u8(OTA_BOOT_NS, OTA_BOOT_FLAG_KEY, &v, 0);
    return v != 0;
}

#ifdef ESP_PLATFORM

#include "bb_ntp.h"
#include "bb_wifi.h"
#include "bb_board.h"
#include "bb_ota_pull.h"
#include "bb_update_check.h"
#include "bb_http.h"
#include "bb_registry.h"
#include "bb_http_client.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#if CONFIG_BB_OTA_BOOT_PROGRESS_HTTP
#include "mdns.h"
#endif

static const char *TAG = "bb_ota_boot";

#define OTA_BOOT_UDP_PORT      9999
#define OTA_BOOT_WORKER_STACK  12288


// Resolve+pull params handed to the worker (single boot-mode run, statics safe).
static const char *s_boot_url   = NULL;
static const char *s_boot_board = NULL;

// Stashed releases_url/board so STATUS_HTTP routes can init bb_update_check
// even when nothing is pending (set in bb_ota_boot_run_if_pending).
#if CONFIG_BB_OTA_BOOT_STATUS_HTTP
#define OTA_BOOT_STATUS_URL_MAX   256
#define OTA_BOOT_STATUS_BOARD_MAX  64
static char s_status_url[OTA_BOOT_STATUS_URL_MAX]     = {0};
static char s_status_board[OTA_BOOT_STATUS_BOARD_MAX] = {0};
static bool s_status_check_initialized                 = false;

// Heap bar for the on-demand check — derives from the shared bb_tls knobs
// (BB_TLS_HEAP_CONTIGUOUS_FLOOR, bridged in bb_tls.h):
//   0 (default) = auto-derive: BB_TLS_SSL_IN_FLOOR
//  >0            = explicit byte override
//  <0            = guard disabled
#if BB_TLS_HEAP_CONTIGUOUS_FLOOR > 0
#  define BB_OTA_HEAP_FLOOR_BYTES ((size_t)BB_TLS_HEAP_CONTIGUOUS_FLOOR)
#elif BB_TLS_HEAP_CONTIGUOUS_FLOOR < 0
#  define BB_OTA_HEAP_FLOOR_BYTES 0   /* guard disabled */
#else
#  define BB_OTA_HEAP_FLOOR_BYTES BB_TLS_SSL_IN_FLOOR
#endif
#define BB_OTA_BOOT_STATUS_MIN_HEAP BB_OTA_HEAP_FLOOR_BYTES

// Ensure bb_update_check is ready for the on-demand routes. Called at init
// (route-registration time) and from run_if_pending after stashing url/board.
static void status_check_ensure_init(void)
{
    if (s_status_check_initialized) return;
    if (s_status_url[0] == '\0') return;  // url not stashed yet
    if (bb_update_check_init(NULL) != BB_OK) return;
    bb_update_check_set_releases_url(s_status_url);
    bb_update_check_set_firmware_board(s_status_board[0] ? s_status_board : NULL);
    s_status_check_initialized = true;
}

// Transient fat-stack task: runs one on-demand manifest check then exits.
static void status_check_task(void *arg)
{
    (void)arg;
    bb_update_check_now();
    vTaskDelete(NULL);
}

// GET /api/update/status — delegates to the shared emitter.
static bb_err_t ota_boot_status_handler(bb_http_request_t *req)
{
    return bb_update_check_emit_status_json(req);
}

// POST /api/update/check — on-demand heap-guarded one-shot check.
static bb_err_t ota_boot_check_handler(bb_http_request_t *req)
{
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");

#if BB_OTA_BOOT_STATUS_MIN_HEAP > 0 || BB_TLS_HEAP_TOTAL_FLOOR > 0
    // Pre-check heap guard: fire check_on_apply (or 503) if either dimension
    // is too low — contiguous block for the mbedTLS IN buffer, total free for
    // the whole handshake transient (~20 KB on esp32-s2-mini).
    {
        size_t largest    = bb_board_heap_internal_largest_free_block();
        size_t total_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const char *dim   = NULL;
        if (!bb_tls_heap_guard_passes(largest, BB_OTA_BOOT_STATUS_MIN_HEAP,
                                       total_free, BB_TLS_HEAP_TOTAL_FLOOR,
                                       &dim)) {
#if CONFIG_BB_OTA_CHECK_ON_APPLY_FALLBACK
            bb_log_w(TAG, "check: %s heap guard failed (largest=%u total_free=%u), "
                     "returning check_on_apply directive",
                     dim, (unsigned)largest, (unsigned)total_free);
            bb_update_check_mark_check_on_apply();
            bb_http_json_obj_stream_t obj;
            bb_http_resp_json_obj_begin(req, &obj);
            bb_http_resp_json_obj_set_str(&obj, "status", "check_on_apply");
            bb_http_resp_json_obj_end(&obj);
            return BB_OK;
#else
            bb_log_w(TAG, "check: %s heap guard failed (largest=%u total_free=%u), skipping",
                     dim, (unsigned)largest, (unsigned)total_free);
            bb_http_resp_set_status(req, 503);
            bb_http_json_obj_stream_t obj;
            bb_http_resp_json_obj_begin(req, &obj);
            bb_http_resp_json_obj_set_str(&obj, "error", "insufficient_heap");
            bb_http_resp_json_obj_end(&obj);
            return BB_OK;
#endif
        }
    }
#endif

    // On single-core (unicore) targets, core 1 does not exist and
    // xTaskCreatePinnedToCore asserts; fall back to no affinity.
    // Default core 1: mirrors bb_ota_pull's s_ota_task_core default, freeing
    // Core 0 for the httpd worker on dual-core targets.
    int status_task_core = 1;
    if (status_task_core >= configNUMBER_OF_CORES) {
        status_task_core = tskNO_AFFINITY;
    }
    if (xTaskCreatePinnedToCore(status_check_task, "ota_status_chk",
                                BB_HTTP_CLIENT_TASK_STACK, NULL, 1, NULL,
                                status_task_core) != pdPASS) {
        bb_log_e(TAG, "check: task create failed");
        bb_http_resp_set_status(req, 503);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "task_create_failed");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    bb_http_resp_set_status(req, 202);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "status", "checking");
    bb_http_resp_json_obj_end(&obj);
    return BB_OK;
}
#endif // CONFIG_BB_OTA_BOOT_STATUS_HTTP

static void breadcrumb(uint8_t stage)
{
    bb_nv_set_u8(OTA_BOOT_NS, OTA_BOOT_STAGE_KEY, stage);
}

// Boot-mode worker: resolve the latest asset + pull it, both at full heap. Runs
// on a fat stack (the mbedTLS handshake in bb_update_check_now needs >=8 KB, more
// than the main task carries). NEVER returns — always reboots.
static void ota_boot_worker(void *arg)
{
    (void)arg;

#if CONFIG_BB_OTA_BOOT_PROGRESS_HTTP
    bb_log_w(TAG, "boot-ota heap: largest_block=%u free_internal=%u",
             (unsigned)bb_board_heap_internal_largest_free_block(),
             (unsigned)bb_board_heap_internal_free());
#endif

    // Stand up bb_update_check synchronously — init + now() run on this stack,
    // no persistent worker spawned, so the runtime can keep it autoregister-off.
    if (bb_update_check_init(NULL) != BB_OK ||
        bb_update_check_set_releases_url(s_boot_url) != BB_OK ||
        bb_update_check_set_firmware_board(s_boot_board) != BB_OK) {
        bb_log_e(TAG, "boot-mode: update_check setup failed, rebooting normal");
        breadcrumb(0xE2);
        bb_ota_emit_progress("boot", BB_OTA_PHASE_FAIL, 0);
        esp_restart();
    }

    bb_update_check_status_t st;
    if (bb_update_check_now() != BB_OK ||
        bb_update_check_get_status(&st) != BB_OK || !st.last_check_ok) {
        bb_log_e(TAG, "boot-mode: manifest check failed, rebooting normal");
        breadcrumb(0xE2);
        bb_ota_emit_progress("boot", BB_OTA_PHASE_FAIL, 0);
        esp_restart();
    }
    if (!st.available || st.download_url[0] == '\0') {
        bb_log_w(TAG, "boot-mode: no update available, rebooting normal");
        breadcrumb(0xE3);
        bb_ota_emit_progress("boot", BB_OTA_PHASE_FAIL, 0);
        esp_restart();
    }
    breadcrumb(4);

#if CONFIG_BB_OTA_BOOT_PROGRESS_HTTP
    bb_log_w(TAG, "boot-ota heap pre-pull: largest_block=%u free_internal=%u",
             (unsigned)bb_board_heap_internal_largest_free_block(),
             (unsigned)bb_board_heap_internal_free());
#endif

    // run_sync drives START/PROGRESS/SUCCESS/FAIL through the forwarded cb.
    bb_log_w(TAG, "boot-mode: pulling %s", st.download_url);
    bb_err_t err = bb_ota_pull_run_sync(st.download_url);
    if (err == BB_OK) {
        breadcrumb(8);
        bb_log_w(TAG, "boot-mode: image written, rebooting to new image");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    bb_log_e(TAG, "boot-mode: pull failed (%s), rebooting normal", esp_err_to_name(err));
    breadcrumb(0xE4);
    esp_restart();
}

#if CONFIG_BB_OTA_BOOT_PROGRESS_HTTP
// GET /api/update/progress — served during boot-mode download window.
// Reads the last emitted progress from bb_ota_hooks (shared stash updated by
// bb_ota_emit_progress); no lock needed (single-writer: the worker task;
// single-reader: the httpd task; both on the same core on single-core targets;
// on dual-core the read is a word-sized load and the worst case is a stale pct,
// which is acceptable for a progress UI).
static bb_err_t ota_boot_progress_handler(bb_http_request_t *req)
{
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");

    bb_ota_phase_t phase;
    int            pct;
    bb_ota_last_progress(&phase, &pct);

    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj,  "state",        bb_ota_boot_phase_str(phase));
    bb_http_resp_json_obj_set_int(&obj,  "progress_pct", pct);
    bb_http_resp_json_obj_set_bool(&obj, "in_progress",  true);
    bb_http_resp_json_obj_end(&obj);
    return BB_OK;
}

// Start the one-route boot-progress HTTP server and advertise mDNS if configured.
// Called from bb_ota_boot_run_if_pending after WiFi+NTP, before spawning the worker.
static void boot_progress_server_start(void)
{
    // Ensure the HTTP server is up. bb_http_server_ensure_started() is the
    // low-level form used here because we're outside the normal registry
    // lifecycle (boot-mode runs before registry init).
    bb_http_reserve_routes(1);  // GET /api/update/progress
    bb_err_t rc = bb_http_server_ensure_started();
    if (rc != BB_OK) {
        bb_log_e(TAG, "boot-progress: http server start failed (%d)", rc);
        return;
    }

    bb_http_handle_t server = bb_http_server_get_handle();
    rc = bb_http_register_route(server, BB_HTTP_GET,
                                "/api/update/progress",
                                ota_boot_progress_handler);
    if (rc != BB_OK) {
        bb_log_e(TAG, "boot-progress: register route failed (%d)", rc);
        return;
    }
    bb_log_i(TAG, "boot-progress: GET /api/update/progress serving");

    // Advertise-only mDNS if the consumer supplied identity via set_mdns_service.
    if (s_mdns_hostname[0] != '\0' && s_mdns_service_type[0] != '\0' && s_mdns_proto[0] != '\0') {
        esp_err_t err = mdns_init();
        if (err != ESP_OK) {
            bb_log_w(TAG, "boot-progress: mdns_init failed (%d), skipping advertise", err);
            return;
        }
        mdns_hostname_set(s_mdns_hostname);
        mdns_service_add(NULL, s_mdns_service_type, s_mdns_proto, s_mdns_port, NULL, 0);
        bb_log_i(TAG, "boot-progress: mDNS advertise-only: %s %s.%s port %u",
                 s_mdns_hostname, s_mdns_service_type, s_mdns_proto, (unsigned)s_mdns_port);
    }

    bb_log_w(TAG, "boot-ota heap: largest_block=%u free_internal=%u",
             (unsigned)bb_board_heap_internal_largest_free_block(),
             (unsigned)bb_board_heap_internal_free());
}
#endif // CONFIG_BB_OTA_BOOT_PROGRESS_HTTP

void bb_ota_boot_run_if_pending(const char *releases_url, const char *board)
{
#if CONFIG_BB_OTA_BOOT_STATUS_HTTP
    // Stash url/board so the on-demand routes have them whether or not a boot
    // is pending. Do this unconditionally before the pending check so status
    // routes work even on a normal (non-pending) boot.
    if (releases_url && releases_url[0] != '\0') {
        strncpy(s_status_url, releases_url, OTA_BOOT_STATUS_URL_MAX - 1);
        s_status_url[OTA_BOOT_STATUS_URL_MAX - 1] = '\0';
    }
    if (board && board[0] != '\0') {
        strncpy(s_status_board, board, OTA_BOOT_STATUS_BOARD_MAX - 1);
        s_status_board[OTA_BOOT_STATUS_BOARD_MAX - 1] = '\0';
    }
    // Init bb_update_check now that we have the url/board (idempotent if
    // already done at route-registration time with a previously stashed url).
    s_status_check_initialized = false;  // force re-init with fresh url/board
    status_check_ensure_init();
#endif

    if (!bb_ota_boot_pending()) {
        return;
    }
    // Clear the one-shot flag FIRST so a failed pull falls back to a normal boot
    // on the next reset (no boot loop).
    bb_nv_set_u8(OTA_BOOT_NS, OTA_BOOT_FLAG_KEY, 0);
    breadcrumb(1);
    bb_ota_emit_progress("boot", BB_OTA_PHASE_START, 0);
    bb_log_w(TAG, "OTA boot-mode: pulling firmware at full heap");

    // Wait for the STA link (caller started WiFi; IP may not be bound yet).
    for (int i = 0; i < 60 && !bb_wifi_has_ip(); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!bb_wifi_has_ip()) {
        bb_log_e(TAG, "OTA boot-mode: no wifi, rebooting normal");
        breadcrumb(0xE1);
        bb_ota_emit_progress("boot", BB_OTA_PHASE_FAIL, 0);
        esp_restart();
    }
    breadcrumb(2);

#if CONFIG_BB_LOG_UDP_SINK
    // Broadcast the boot-mode log trace (serial-less boards). 0xFFFFFFFF =
    // 255.255.255.255 (subnet broadcast). Listen with: nc -ul <port>.
    bb_log_udp_enable(0xFFFFFFFFU, OTA_BOOT_UDP_PORT);
#endif

    // TLS cert validation needs a real clock — a 1970 epoch reads as "cert not
    // yet valid". Start NTP and wait; proceed regardless so a sync miss surfaces
    // as a visible cert error rather than a silent hang.
    bb_ntp_start(NULL);
    if (!bb_ntp_wait_synced(15000)) {
        bb_log_w(TAG, "OTA boot-mode: SNTP timeout, proceeding (TLS may fail)");
    }
    breadcrumb(3);

#if CONFIG_BB_OTA_BOOT_PROGRESS_HTTP
    boot_progress_server_start();
#endif

    // Hand the resolve + pull to a fat-stack worker (the TLS handshakes need more
    // stack than the main task carries). This task then blocks; the worker always reboots.
    s_boot_url   = releases_url;
    s_boot_board = board;
    if (xTaskCreate(ota_boot_worker, "ota_boot", OTA_BOOT_WORKER_STACK, NULL, 5, NULL) != pdPASS) {
        bb_log_e(TAG, "OTA boot-mode: worker create failed, rebooting normal");
        breadcrumb(0xE5);
        bb_ota_emit_progress("boot", BB_OTA_PHASE_FAIL, 0);
        esp_restart();
    }
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

/* Reboot shortly after the route's 202 response flushes. */
static void ota_boot_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

// POST /api/update/apply — arm boot mode and reboot. The lean trigger: one route,
// no download worker until the next boot. On a boot-mode board this is the single
// owner of /api/update/apply (bb_ota_pull does not register it); the distinct
// "rebooting_for_boot_mode_ota" status tells clients to wait for the device to
// reappear on a bumped version rather than poll /api/update/progress.
static bb_err_t ota_boot_handler(bb_http_request_t *req)
{
    bb_ota_boot_arm();
    bb_log_w(TAG, "OTA boot-mode armed via /api/update/apply; rebooting");
    bb_http_resp_set_status(req, 202);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "status", "rebooting_for_boot_mode_ota");
    bb_http_resp_json_obj_end(&obj);
    xTaskCreate(ota_boot_reboot_task, "ota_boot_rb", 2048, NULL, 5, NULL);
    return BB_OK;
}

bb_err_t bb_ota_boot_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    static const bb_route_response_t s_responses[] = {
        { 202, "application/json",
          "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\"}}}",
          "boot mode armed; rebooting" },
        { 0 },
    };
    static const bb_route_t s_route = {
        .method    = BB_HTTP_POST,
        .path      = "/api/update/apply",
        .tag       = "update",
        .summary   = "Apply update via OTA boot mode: arm + reboot (full-heap pull next boot)",
        .responses = s_responses,
        .handler   = ota_boot_handler,
    };

    bb_err_t rc = bb_http_register_described_route(server, &s_route);
    if (rc != BB_OK) {
        bb_log_e(TAG, "register /api/update/apply failed: %d", rc);
        return rc;
    }
    bb_log_i(TAG, "OTA boot-mode apply route registered");

#if CONFIG_BB_OTA_BOOT_STATUS_HTTP
    // Attempt early init of bb_update_check in case run_if_pending was already
    // called (url/board stashed). If not yet called, init will complete once
    // run_if_pending stashes the url/board.
    status_check_ensure_init();

    static const bb_route_response_t s_status_responses[] = {
        { 200, "application/json",
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
                         "\"no_asset\",\"check_failed\",\"check_on_apply\"]},"
             "\"last_check_ts\":{\"type\":\"integer\"}},"
           "\"required\":[\"current\",\"latest\",\"download_url\","
                         "\"available\",\"last_check_ok\",\"enabled\",\"outcome\"]}",
          "current status of the update poller" },
        { 503, "application/json", NULL, "bb_update_check not initialized" },
        { 0 },
    };
    static const bb_route_t s_status_route = {
        .method    = BB_HTTP_GET,
        .path      = "/api/update/status",
        .tag       = "update",
        .summary   = "Latest known release-check state (boot-mode on-demand)",
        .responses = s_status_responses,
        .handler   = ota_boot_status_handler,
    };

    static const bb_route_response_t s_check_responses[] = {
        { 200, "application/json",
          "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\",\"enum\":[\"checking\",\"check_on_apply\"]}}}",
          "check_on_apply directive (boot-mode heap fallback): POST /api/update/apply directly" },
        { 202, "application/json",
          "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\"}}}",
          "check triggered; poll GET /api/update/status for result" },
        { 503, "application/json",
          "{\"type\":\"object\",\"properties\":{\"error\":{\"type\":\"string\"}}}",
          "insufficient heap or task creation failed" },
        { 0 },
    };
    static const bb_route_t s_check_route = {
        .method    = BB_HTTP_POST,
        .path      = "/api/update/check",
        .tag       = "update",
        .summary   = "Trigger an on-demand update check (boot-mode boards)",
        .responses = s_check_responses,
        .handler   = ota_boot_check_handler,
    };

    rc = bb_http_register_described_route(server, &s_status_route);
    if (rc != BB_OK) {
        bb_log_e(TAG, "register /api/update/status failed: %d", rc);
        return rc;
    }
    rc = bb_http_register_described_route(server, &s_check_route);
    if (rc != BB_OK) {
        bb_log_e(TAG, "register /api/update/check failed: %d", rc);
        return rc;
    }
    bb_log_i(TAG, "OTA boot-mode status routes registered");
#endif // CONFIG_BB_OTA_BOOT_STATUS_HTTP

    return BB_OK;
}

#if CONFIG_BB_OTA_BOOT_AUTOREGISTER
static bb_err_t bb_ota_boot_reserve_routes(void)
{
    bb_http_reserve_routes(1);  // POST /api/update/apply
    return BB_OK;
}
BB_REGISTRY_REGISTER_PRE_HTTP(bb_ota_boot, bb_ota_boot_reserve_routes);
BB_REGISTRY_REGISTER_N(bb_ota_boot, bb_ota_boot_init, 1);
#endif

#endif // ESP_PLATFORM

#include "bb_ota_boot.h"
#include "bb_tls.h"
#include "bb_ota_hooks.h"
#include "bb_log.h"
#include "bb_config.h"
#include "bb_nv_namespaces.h"
#include "bb_meminfo.h"
#include "bb_str.h"

#include <string.h>

// Flag lives in the bb_cfg namespace, round-tripped through bb_config
// (typed layer over bb_storage) rather than bb_nv's generic KV forwarder
// (B1-756, bb_nv dissolution epic B1-708) — bb_config's U8 encoding resolves
// to the SAME nvs_get_u8/nvs_set_u8 calls bb_nv_get_u8/set_u8 made (both are
// thin forwarders to bb_storage_nvs, see bb_storage_nvs.h), so the
// namespace/key/U8-typed on-flash format below is byte-compatible with what
// this component previously read/wrote via bb_nv. The flag is a one-shot
// mechanism marker, NOT a user config key — deliberately not in the
// bb_nv_config manifest table.
#define OTA_BOOT_NS         BB_NV_CONFIG_NVS_NS
#define OTA_BOOT_FLAG_KEY   "ota_boot_mode"

static const bb_config_field_t s_ota_boot_flag_field = {
    .id          = "ota_boot.flag",
    .type        = BB_CONFIG_U8,
    .addr        = { .backend = "nvs", .ns_or_dir = OTA_BOOT_NS, .key = OTA_BOOT_FLAG_KEY },
    .def         = { .u8 = 0 },
    .has_default = true,
};

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
    bb_strlcpy(s_mdns_hostname, hostname, sizeof(s_mdns_hostname));
    bb_strlcpy(s_mdns_service_type, service_type, sizeof(s_mdns_service_type));
    bb_strlcpy(s_mdns_proto, proto, sizeof(s_mdns_proto));
    s_mdns_port = port;
    return BB_OK;
}

void bb_ota_boot_arm(void)
{
    bb_config_set_u8(&s_ota_boot_flag_field, 1);
}

bool bb_ota_boot_pending(void)
{
    uint8_t v = 0;
    bb_config_get_u8(&s_ota_boot_flag_field, &v);
    return v != 0;
}

#ifdef ESP_PLATFORM

#include "bb_ntp.h"
#include "bb_wifi.h"
#include "bb_ota_pull.h"
#include "bb_ota_check.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_client.h"
#include "bb_task.h"
#include "bb_system.h"
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

// Stashed releases_url/board so STATUS_HTTP routes can init bb_ota_check
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

// Ensure bb_ota_check is ready for the on-demand routes. Called at init
// (route-registration time) and from run_if_pending after stashing url/board.
static void status_check_ensure_init(void)
{
    if (s_status_check_initialized) return;
    if (s_status_url[0] == '\0') return;  // url not stashed yet
    if (bb_ota_check_init(NULL) != BB_OK) return;
    bb_ota_check_set_releases_url(s_status_url);
    bb_ota_check_set_firmware_board(s_status_board[0] ? s_status_board : NULL);
    s_status_check_initialized = true;
}

// ota_boot_status_handler/ota_boot_check_handler are only wired up inside
// bb_ota_boot_init below, which is itself gated on BB_OTA_STRATEGY_BOOT (see
// bb_ota_boot.h) — guard them the same way so they are not left as unused
// statics when the boot strategy is off but STATUS_HTTP is on.
#if defined(CONFIG_BB_OTA_STRATEGY_BOOT) && CONFIG_BB_OTA_STRATEGY_BOOT

// GET /api/update/status — delegates to the shared emitter.
static bb_err_t ota_boot_status_handler(bb_http_request_t *req)
{
    return bb_ota_check_emit_status_json(req);
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
        bb_meminfo_snapshot_t mem;
        bb_meminfo_get(&mem);
        size_t largest    = mem.internal.largest_free_block;
        size_t total_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const char *dim   = NULL;
        if (!bb_tls_heap_guard_passes(largest, BB_OTA_BOOT_STATUS_MIN_HEAP,
                                       total_free, BB_TLS_HEAP_TOTAL_FLOOR,
                                       &dim)) {
#if CONFIG_BB_OTA_CHECK_ON_APPLY_FALLBACK
            bb_log_w(TAG, "check: %s heap guard failed (largest=%u total_free=%u), "
                     "returning check_on_apply directive",
                     dim, (unsigned)largest, (unsigned)total_free);
            bb_ota_check_mark_check_on_apply();
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

    // Delegate to the shared on-demand worker (bb_ota_check_espidf.c) instead
    // of spawning a dedicated task here — that worker already carries a
    // correctly sized/prioritized stack (consumer-tunable via
    // bb_ota_check_set_task_core/priority), avoiding the hardcoded prio-1/
    // core-1 starvation bug this handler previously had on non-PSRAM boards
    // where core 1 also hosts other CPU-bound work.
    if (bb_ota_check_kick() == BB_OK) {
        bb_http_resp_set_status(req, 202);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "status", "checking");
        bb_http_resp_json_obj_end(&obj);
        return BB_OK;
    }

    bb_log_e(TAG, "check: kick unavailable");
    bb_http_resp_set_status(req, 503);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "error", "check_unavailable");
    bb_http_resp_json_obj_end(&obj);
    return BB_OK;
}
#endif // defined(CONFIG_BB_OTA_STRATEGY_BOOT) && CONFIG_BB_OTA_STRATEGY_BOOT
#endif // CONFIG_BB_OTA_BOOT_STATUS_HTTP

// Boot-mode worker: resolve the latest asset + pull it, both at full heap. Runs
// on a fat stack (the mbedTLS handshake in bb_ota_check_now needs >=8 KB, more
// than the main task carries). NEVER returns — always reboots.
static void ota_boot_worker(void *arg)
{
    (void)arg;

#if CONFIG_BB_OTA_BOOT_PROGRESS_HTTP
    {
        bb_meminfo_snapshot_t mem;
        bb_meminfo_get(&mem);
        bb_log_w(TAG, "boot-ota heap: largest_block=%u free_internal=%u",
                 (unsigned)mem.internal.largest_free_block,
                 (unsigned)mem.internal.free);
    }
#endif

    // Stand up bb_ota_check synchronously — init + now() run on this stack,
    // no persistent worker spawned, so the runtime can keep it autoregister-off.
    if (bb_ota_check_init(NULL) != BB_OK ||
        bb_ota_check_set_releases_url(s_boot_url) != BB_OK ||
        bb_ota_check_set_firmware_board(s_boot_board) != BB_OK) {
        bb_log_e(TAG, "boot-mode: update_check setup failed, rebooting normal");
        bb_ota_emit_progress("boot", BB_OTA_PHASE_FAIL, 0);
        bb_system_restart_reason(BB_RESET_SRC_OTA_BOOT_ABORT, "init_fail");
    }

    bb_ota_check_status_t st;
    if (bb_ota_check_now() != BB_OK ||
        bb_ota_check_get_status(&st) != BB_OK || !st.last_check_ok) {
        bb_log_e(TAG, "boot-mode: manifest check failed, rebooting normal");
        bb_ota_emit_progress("boot", BB_OTA_PHASE_FAIL, 0);
        bb_system_restart_reason(BB_RESET_SRC_OTA_BOOT_ABORT, "manifest_fail");
    }
    if (!st.available || st.download_url[0] == '\0') {
        bb_log_w(TAG, "boot-mode: no update available, rebooting normal");
        bb_ota_emit_progress("boot", BB_OTA_PHASE_FAIL, 0);
        bb_system_restart_reason(BB_RESET_SRC_OTA_BOOT_ABORT, "no_update");
    }

#if CONFIG_BB_OTA_BOOT_PROGRESS_HTTP
    {
        bb_meminfo_snapshot_t mem;
        bb_meminfo_get(&mem);
        bb_log_w(TAG, "boot-ota heap pre-pull: largest_block=%u free_internal=%u",
                 (unsigned)mem.internal.largest_free_block,
                 (unsigned)mem.internal.free);
    }
#endif

    // run_sync drives START/PROGRESS/SUCCESS/FAIL through the forwarded cb.
    bb_log_w(TAG, "boot-mode: pulling %s", st.download_url);
    bb_err_t err = bb_ota_pull_run_sync(st.download_url);
    if (err == BB_OK) {
        bb_log_w(TAG, "boot-mode: image written, rebooting to new image");
        vTaskDelay(pdMS_TO_TICKS(500));
        bb_system_restart_reason(BB_RESET_SRC_OTA_BOOT_DONE, NULL);
    }

    bb_log_e(TAG, "boot-mode: pull failed (%s), rebooting normal", esp_err_to_name(err));
    bb_system_restart_reason(BB_RESET_SRC_OTA_BOOT_ABORT, "pull_fail");
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
                                BB_ROUTE_UPDATE_PROGRESS,
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

    {
        bb_meminfo_snapshot_t mem;
        bb_meminfo_get(&mem);
        bb_log_w(TAG, "boot-ota heap: largest_block=%u free_internal=%u",
                 (unsigned)mem.internal.largest_free_block,
                 (unsigned)mem.internal.free);
    }
}
#endif // CONFIG_BB_OTA_BOOT_PROGRESS_HTTP

void bb_ota_boot_run_if_pending(const char *releases_url, const char *board)
{
#if CONFIG_BB_OTA_BOOT_STATUS_HTTP
    // Stash url/board so the on-demand routes have them whether or not a boot
    // is pending. Do this unconditionally before the pending check so status
    // routes work even on a normal (non-pending) boot.
    if (releases_url && releases_url[0] != '\0') {
        bb_strlcpy(s_status_url, releases_url, sizeof(s_status_url));
    }
    if (board && board[0] != '\0') {
        bb_strlcpy(s_status_board, board, sizeof(s_status_board));
    }
    // Init bb_ota_check now that we have the url/board (idempotent if
    // already done at route-registration time with a previously stashed url).
    s_status_check_initialized = false;  // force re-init with fresh url/board
    status_check_ensure_init();
#endif

    if (!bb_ota_boot_pending()) {
        return;
    }
    // Clear the one-shot flag FIRST so a failed pull falls back to a normal boot
    // on the next reset (no boot loop).
    bb_config_set_u8(&s_ota_boot_flag_field, 0);
    bb_ota_emit_progress("boot", BB_OTA_PHASE_START, 0);
    bb_log_w(TAG, "OTA boot-mode: pulling firmware at full heap");

    // Wait for the STA link (caller started WiFi; IP may not be bound yet).
    for (int i = 0; i < 60 && !bb_wifi_has_ip(); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!bb_wifi_has_ip()) {
        bb_log_e(TAG, "OTA boot-mode: no wifi, rebooting normal");
        bb_ota_emit_progress("boot", BB_OTA_PHASE_FAIL, 0);
        bb_system_restart_reason(BB_RESET_SRC_OTA_BOOT_ABORT, "no_wifi");
    }

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

#if CONFIG_BB_OTA_BOOT_PROGRESS_HTTP
    boot_progress_server_start();
#endif

    // Hand the resolve + pull to a fat-stack worker (the TLS handshakes need more
    // stack than the main task carries). This task then blocks; the worker always reboots.
    s_boot_url   = releases_url;
    s_boot_board = board;
    TaskHandle_t boot_worker_task = NULL;
    bb_task_config_t boot_cfg = {
        .entry       = ota_boot_worker,
        .name        = "ota_boot",
        .arg         = NULL,
        .stack_bytes = OTA_BOOT_WORKER_STACK,
        .priority    = 5,
        .core        = BB_TASK_CORE_ANY,
        .backing     = BB_TASK_BACKING_DYNAMIC,
        .wdt_arm     = false,
    };
    if (bb_task_create(&boot_cfg, (void **)&boot_worker_task) != BB_OK) {
        bb_log_e(TAG, "OTA boot-mode: worker create failed, rebooting normal");
        bb_ota_emit_progress("boot", BB_OTA_PHASE_FAIL, 0);
        bb_system_restart_reason(BB_RESET_SRC_OTA_BOOT_ABORT, "task_fail");
    }
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

// bb_ota_boot_init and its exclusive helpers below are the single registrant
// of POST /api/update/apply, chosen by the BB_OTA_STRATEGY Kconfig choice
// (see the matching gate + rationale in bb_ota_boot.h). When the boot
// strategy is not selected, bb_ota_boot.h's no-op stub satisfies the
// generated composition call and none of this is compiled in.
#if defined(CONFIG_BB_OTA_STRATEGY_BOOT) && CONFIG_BB_OTA_STRATEGY_BOOT

/* Reboot shortly after the route's 202 response flushes. */
static void ota_boot_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    bb_system_restart_reason(BB_RESET_SRC_OTA_BOOT_APPLY, NULL);
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
    TaskHandle_t reboot_task = NULL;
    bb_task_config_t reboot_cfg = {
        .entry       = ota_boot_reboot_task,
        .name        = "ota_boot_rb",
        .arg         = NULL,
        .stack_bytes = 2048,
        .priority    = 5,
        .core        = BB_TASK_CORE_ANY,
        .backing     = BB_TASK_BACKING_DYNAMIC,
        .wdt_arm     = false,
    };
    bb_task_create(&reboot_cfg, (void **)&reboot_task);
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
        .path      = BB_ROUTE_UPDATE_APPLY,
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
    // Attempt early init of bb_ota_check in case run_if_pending was already
    // called (url/board stashed). If not yet called, init will complete once
    // run_if_pending stashes the url/board.
    status_check_ensure_init();

    static const bb_route_response_t s_status_responses[] = {
        { 200, "application/json",
          "{\"type\":\"object\","
           "\"properties\":{"
             "\"ts_ms\":{\"type\":\"integer\"},"
             "\"data\":{\"type\":\"object\","
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
                             "\"available\",\"last_check_ok\",\"enabled\",\"outcome\"]}},"
           "\"required\":[\"ts_ms\",\"data\"]}",
          "BREAKING (B1-1053 PR3): response root changed from a bare object "
          "to the {ts_ms,data} envelope -- \\\"data\\\" carries exactly the "
          "fields this route used to emit at the root. ts_ms is the "
          "wall-clock time (ms) this response was rendered, NOT a sample "
          "time. data is the current status of the update poller "
          "(boot-mode on-demand)." },
        { 503, "application/json", NULL, "bb_ota_check not initialized" },
        { 0 },
    };
    static const bb_route_t s_status_route = {
        .method    = BB_HTTP_GET,
        .path      = BB_ROUTE_UPDATE_STATUS,
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
          "insufficient heap for the TLS handshake, or check unavailable (bb_ota_check not "
          "initialized, or a manifest check is already in flight)" },
        { 0 },
    };
    static const bb_route_t s_check_route = {
        .method    = BB_HTTP_POST,
        .path      = BB_ROUTE_UPDATE_CHECK,
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

#endif // defined(CONFIG_BB_OTA_STRATEGY_BOOT) && CONFIG_BB_OTA_STRATEGY_BOOT

bb_err_t bb_ota_boot_reserve_routes(void)
{
    bb_http_reserve_routes(1);  // POST /api/update/apply
    return BB_OK;
}

#endif // ESP_PLATFORM

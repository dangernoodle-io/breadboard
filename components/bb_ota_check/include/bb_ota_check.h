#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "bb_core.h"

// Minimum buffer size for version strings in bb_ota_check_status_t.
// The longest dev format is "dev-<7sha>+<4hash>-bb-<7sha>+<4hash>" = 36 chars;
// size to 48 to leave headroom. (TA-462: was 24, causing truncation.)
#define BB_OTA_CHECK_VERSION_BUF 48
#include "bb_release_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

// Shared /api/update/* route-path constants. Referenced by bb_ota_pull,
// bb_ota_boot, and bb_ota_check_espidf (all already depend on this
// header) so the four paths cannot drift independently across route
// registration sites. Values are the public HTTP contract — do not change
// without a coordinated consumer update (taipan-cli, webui).
#define BB_ROUTE_UPDATE_APPLY    "/api/update/apply"
#define BB_ROUTE_UPDATE_CHECK    "/api/update/check"
#define BB_ROUTE_UPDATE_PROGRESS "/api/update/progress"
#define BB_ROUTE_UPDATE_STATUS   "/api/update/status"

// bb_ota_check — periodically poll a release manifest URL, compare semver
// to the running firmware version, and on state change bump the
// `update.available` bb_data generation plus update an mDNS TXT key
// `update=<value>`.
//
// Values for the mDNS `update=` TXT and /api/update/status `available` field:
//   - `unknown` pre-check (status.available=false, latest="")
//   - `none`    up to date (status.available=false, latest=current)
//   - `<tag>`   newer release (status.available=true, latest=<tag>)
//
// The parser is overridable: default is bb_release_manifest_parse_github;
// consumers can swap in their own (e.g. GitLab) via bb_ota_check_set_parser.

// Terminal outcome of the last update check.
//
// Distinct from last_check_ok:
//   UP_TO_DATE, AVAILABLE, NO_ASSET → last_check_ok=true (check succeeded)
//   FAILED                          → last_check_ok=false
//   UNKNOWN                         → initial state, no check has run
//
// NO_ASSET means the release was parsed successfully but no <board>.bin asset
// was found for this board. It is a SUCCESS outcome (last_check_ok=true) so
// consumers polling last_check_ok do not hang waiting for a check that never
// transitions to OK. available=false and download_url="" for NO_ASSET.
typedef enum {
    BB_OTA_CHECK_OUTCOME_UNKNOWN = 0,
    BB_OTA_CHECK_OUTCOME_UP_TO_DATE,
    BB_OTA_CHECK_OUTCOME_AVAILABLE,
    BB_OTA_CHECK_OUTCOME_NO_ASSET,
    BB_OTA_CHECK_OUTCOME_FAILED,
    BB_OTA_CHECK_OUTCOME_CHECK_ON_APPLY, // heap-deferred: board checks during apply
} bb_ota_check_outcome_t;

// JSON-Schema enum literal for the /api/update/status "outcome" field.
// MUST stay byte-identical (values and order) to outcome_str() in
// bb_ota_check_common.c. Referenced by both /api/update/status schema
// sites (bb_ota_check_espidf.c and bb_ota_boot.c) so they cannot drift.
#define BB_OTA_CHECK_OUTCOME_ENUM_JSON \
    "\"unknown\",\"up_to_date\",\"available\"," \
    "\"no_asset\",\"check_failed\",\"check_on_apply\""

typedef struct {
    uint32_t interval_s;     // 0 -> CONFIG_BB_OTA_CHECK_INTERVAL_S (21600 s = 6 h)
    bool     post_initial;   // post update.available on first successful check even if up to date
} bb_ota_check_cfg_t;

typedef struct {
    char    current[BB_OTA_CHECK_VERSION_BUF];
    char    latest[BB_OTA_CHECK_VERSION_BUF];
    char    download_url[256];
    char    board[64];         // effective board name used for the last check ("unknown" if unset)
    int64_t last_check_us;     // epoch-us (gettimeofday), NOT monotonic; 0 if never
    bool    last_check_ok;     // false => sticky failure (FAILED outcome)
    bool    available;
    bool    enabled;           // mirrors bb_settings_update_check_enabled_get()
    bb_ota_check_outcome_t outcome; // terminal outcome of the last check
} bb_ota_check_status_t;

// Idempotent. cfg=NULL uses Kconfig defaults.
// NOTE: bb_ota_check_init does NOT publish an initial snapshot; callers must
// call bb_ota_check_publish_initial() (after the composition root has
// bb_data_bind()'d/bb_data_http_attach()'d "update.available") so a consumer
// connecting before the first periodic check sees the last known state.
bb_err_t bb_ota_check_init(const bb_ota_check_cfg_t *cfg);

// Publish an initial snapshot to the update.available bb_data key. Must be
// called after bb_ota_check_init. This ensures a consumer connecting before
// the first periodic check (up to CONFIG_BB_OTA_CHECK_INTERVAL_S seconds)
// sees this entry rather than empty state. The initial state has
// available=false, current=<running>, latest="", download_url="",
// last_check_ok=false. Returns BB_ERR_INVALID_STATE if init hasn't run.
bb_err_t bb_ota_check_publish_initial(void);

// Releases URL (string is copied into a fixed-size internal buffer).
bb_err_t bb_ota_check_set_releases_url(const char *url);

// Override the manifest parser. Pass NULL to restore the default (GitHub).
bb_err_t bb_ota_check_set_parser(bb_release_manifest_parse_fn fn);

// Set the firmware board name used when matching release assets.
// The default is "unknown" (matches "unknown.bin"). Pass the board prefix
// without the ".bin" suffix (e.g. "taipanminer-tdongle-s3"). Pass NULL or ""
// to revert to the default. The string is copied into a fixed-size buffer.
// Returns BB_ERR_INVALID_STATE if called before bb_ota_check_init,
// BB_ERR_INVALID_ARG if the string is too long (> 63 chars).
bb_err_t bb_ota_check_set_firmware_board(const char *board);

// Hook functions invoked around each manifest fetch. Matches the bb_ota_pull
// hook shape so consumers can pass the same mining_pause / mining_resume.
// pause returns true on success; if false, the fetch is skipped and resume
// is NOT called.
typedef bb_http_pause_cb_t  bb_ota_check_pause_cb_t;
typedef bb_http_resume_cb_t bb_ota_check_resume_cb_t;

// Set optional pause/resume hooks. pause is called just before
// bb_http_client_get_stream; resume is called immediately after (success or
// failure). Pass NULL for either to disable. Returns BB_ERR_INVALID_STATE if
// called before bb_ota_check_init.
bb_err_t bb_ota_check_set_hooks(bb_ota_check_pause_cb_t pause,
                                   bb_ota_check_resume_cb_t resume);

// Configure which FreeRTOS core the bb_ota_check one-shot worker spawns on.
// Default is Core 1 — matches bb_ota_pull_set_task_core's default and
// keeps the mbedTLS handshake off the core that carries lwip/wifi/httpd.
// Pass tskNO_AFFINITY (-1) to let FreeRTOS schedule, or 0 to force Core 0.
// Takes effect on the next spawn (can be changed at any time).
void bb_ota_check_set_task_core(int core);

// Configure the FreeRTOS priority of the bb_ota_check one-shot worker task.
// Default is 1 — low so the worker yields to application tasks. Consumers
// pinning the worker to the same core as a high-priority CPU-bound task
// (e.g. a mining hashloop) need to raise the priority above that task's,
// otherwise the spawned task never gets CPU to call the pause hook.
// Takes effect on the next spawn (can be changed at any time).
void bb_ota_check_set_task_priority(int priority);

// Trigger an immediate synchronous check on the caller's stack.
// Runs the full manifest fetch and parsing synchronously. Use only from contexts
// with ≥8 KB stack (e.g. the worker task itself or test harnesses).
// HTTP handlers and other stack-constrained code should use bb_ota_check_kick().
bb_err_t bb_ota_check_now(void);

// Kick the bb_ota_check worker to run a check on its own task stack.
// Non-blocking: returns immediately. Use this from HTTP handlers or other
// stack-constrained contexts. The result lands in bb_ota_check_get_status()
// after the worker completes; subscribers to update.available will see a post
// if the check transitions state.
// This function is ESP-IDF only; host/Arduino backends provide a synchronous stub.
bb_err_t bb_ota_check_kick(void);

// Kick the worker and block until the check completes or timeout_ms elapses.
// On ESP-IDF: kicks the worker semaphore, then polls last_check_us advancing
// (sampled before the kick) with 100 ms sleeps, so the httpd request task
// blocks but other sockets are unaffected. The worker runs on its own stack
// so there is no stack-overflow risk from this call site.
// On host/Arduino: synchronous stub that calls bb_ota_check_now() directly.
// Returns BB_OK if the check completed within the timeout,
// BB_ERR_TIMEOUT if timeout_ms elapsed before the worker finished,
// BB_ERR_INVALID_STATE if init hasn't run.
bb_err_t bb_ota_check_run_blocking(uint32_t timeout_ms);

// Copy the latest status snapshot into out. BB_ERR_INVALID_ARG if out is NULL,
// BB_ERR_INVALID_STATE if init hasn't run.
bb_err_t bb_ota_check_get_status(bb_ota_check_status_t *out);

// Mark the cached status as deferred-to-apply: sets outcome=check_on_apply,
// available=false, last_check_ok=false. Used by boot-mode boards when the heap
// guard fires and CONFIG_BB_OTA_CHECK_ON_APPLY_FALLBACK is enabled. Leaves
// current/latest/download_url/last_check_us unchanged.
// Returns BB_ERR_INVALID_STATE if init hasn't run.
bb_err_t bb_ota_check_mark_check_on_apply(void);

// Serialize the current bb_ota_check status to JSON and send it as an HTTP
// response body. Emits the same JSON shape as GET /api/update/status (fields:
// current, latest, download_url, available, last_check_ok, enabled, outcome,
// last_check_ts). Sets CORS headers. Returns 503 with {"error":"not initialized"}
// if bb_ota_check_init has not been called. This is the shared emitter used
// by both the persistent bb_ota_check route and the boot-mode on-demand route
// (bb_ota_boot with CONFIG_BB_OTA_BOOT_STATUS_HTTP); the JSON shape is the
// public contract for taipan-cli and the webui — do not change without a
// coordinated consumer update.
bb_err_t bb_ota_check_emit_status_json(bb_http_request_t *req);

/* Reserve route-table slots for bb_ota_check before the HTTP server starts. */
// bbtool:init tier=pre_http fn=bb_ota_check_reserve_routes
bb_err_t bb_ota_check_reserve_routes(void);

/* Registry hook — inits bb_ota_check and registers GET /api/update/status +
 * the update-config routes. */
// bbtool:init tier=regular fn=bb_ota_check_register_init server=true
bb_err_t bb_ota_check_register_init(bb_http_handle_t server);

// OTA operation exclusive-slot claim. Backed by bb_claim; at most one OTA-class
// operation (ota_pull download or upd_check manifest fetch) runs at a time.
// acquire → BB_OK (free or same id), BB_ERR_CONFLICT (different id).
// release → no-op if not held by id.
bb_err_t bb_ota_check_ota_claim_acquire(const char *id);
void     bb_ota_check_ota_claim_release(const char *id);

#ifdef BB_OTA_CHECK_TESTING
void bb_ota_check_ota_claim_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "bb_core.h"
#include "bb_release_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

// bb_update_check — periodically poll a release manifest URL, compare semver
// to the running firmware version, and on state change post a bb_event topic
// `update.available` plus update an mDNS TXT key `update=<value>`.
//
// Values for the mDNS `update=` TXT and /api/update/status `available` field:
//   - `unknown` pre-check (status.available=false, latest="")
//   - `none`    up to date (status.available=false, latest=current)
//   - `<tag>`   newer release (status.available=true, latest=<tag>)
//
// The parser is overridable: default is bb_release_manifest_parse_github;
// consumers can swap in their own (e.g. GitLab) via bb_update_check_set_parser.

typedef struct {
    uint32_t interval_s;     // 0 -> CONFIG_BB_UPDATE_CHECK_INTERVAL_S (21600 s = 6 h)
    bool     post_initial;   // post update.available on first successful check even if up to date
} bb_update_check_cfg_t;

typedef struct {
    char    current[24];
    char    latest[24];
    char    download_url[256];
    int64_t last_check_us;     // 0 if never
    bool    last_check_ok;     // false => sticky failure
    bool    available;
    bool    enabled;           // mirrors bb_nv_config_update_check_enabled()
} bb_update_check_status_t;

// Idempotent. cfg=NULL uses Kconfig defaults.
// NOTE: bb_update_check_init does NOT post an initial snapshot; callers must
// call bb_update_check_publish_initial() after attaching a ring to the topic
// (via bb_event_routes_attach_ex or bb_event_ring_attach_ex) to ensure SSE
// clients connecting before the first periodic check see the last known state.
bb_err_t bb_update_check_init(const bb_update_check_cfg_t *cfg);

// Publish an initial snapshot to the update.available topic. Must be called
// after bb_update_check_init and after attaching a ring/routes (e.g. via
// bb_event_routes_attach_ex(..., true) with retained=true). This ensures SSE
// clients connecting before the first periodic check (up to
// CONFIG_BB_UPDATE_CHECK_INTERVAL_S seconds) replay this entry rather than
// seeing empty state. The initial state has available=false, current=<running>,
// latest="", download_url="", last_check_ok=false. Returns BB_ERR_INVALID_STATE
// if init hasn't run.
bb_err_t bb_update_check_publish_initial(void);

// Releases URL (string is copied into a fixed-size internal buffer).
bb_err_t bb_update_check_set_releases_url(const char *url);

// Override the manifest parser. Pass NULL to restore the default (GitHub).
bb_err_t bb_update_check_set_parser(bb_release_manifest_parse_fn fn);

// Set the firmware board name used when matching release assets.
// The default is "firmware" (matches "firmware.bin"). Pass the board prefix
// without the ".bin" suffix (e.g. "taipanminer-tdongle-s3"). Pass NULL or ""
// to revert to the default. The string is copied into a fixed-size buffer.
// Returns BB_ERR_INVALID_STATE if called before bb_update_check_init,
// BB_ERR_INVALID_ARG if the string is too long (> 63 chars).
bb_err_t bb_update_check_set_firmware_board(const char *board);

// Hook functions invoked around each manifest fetch. Matches the bb_ota_pull
// hook shape so consumers can pass the same mining_pause / mining_resume.
// pause returns true on success; if false, the fetch is skipped and resume
// is NOT called.
typedef bool (*bb_update_check_pause_cb_t)(void);
typedef void (*bb_update_check_resume_cb_t)(void);

// Set optional pause/resume hooks. pause is called just before
// bb_http_client_get_stream; resume is called immediately after (success or
// failure). Pass NULL for either to disable. Returns BB_ERR_INVALID_STATE if
// called before bb_update_check_init.
bb_err_t bb_update_check_set_hooks(bb_update_check_pause_cb_t pause,
                                   bb_update_check_resume_cb_t resume);

// Trigger an immediate synchronous check on the caller's stack.
// Runs the full manifest fetch and parsing synchronously. Use only from contexts
// with ≥8 KB stack (e.g. the worker task itself or test harnesses).
// HTTP handlers and other stack-constrained code should use bb_update_check_kick().
bb_err_t bb_update_check_now(void);

// Kick the bb_update_check worker to run a check on its own task stack.
// Non-blocking: returns immediately. Use this from HTTP handlers or other
// stack-constrained contexts. The result lands in bb_update_check_get_status()
// after the worker completes; subscribers to update.available will see a post
// if the check transitions state.
// This function is ESP-IDF only; host/Arduino backends provide a synchronous stub.
bb_err_t bb_update_check_kick(void);

// Copy the latest status snapshot into out. BB_ERR_INVALID_ARG if out is NULL,
// BB_ERR_INVALID_STATE if init hasn't run.
bb_err_t bb_update_check_get_status(bb_update_check_status_t *out);

#ifdef __cplusplus
}
#endif

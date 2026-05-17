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
} bb_update_check_status_t;

// Idempotent. cfg=NULL uses Kconfig defaults.
bb_err_t bb_update_check_init(const bb_update_check_cfg_t *cfg);

// Releases URL (string is copied into a fixed-size internal buffer).
bb_err_t bb_update_check_set_releases_url(const char *url);

// Override the manifest parser. Pass NULL to restore the default (GitHub).
bb_err_t bb_update_check_set_parser(bb_release_manifest_parse_fn fn);

// Hook functions invoked around each manifest fetch.
typedef void (*bb_update_check_hook_fn)(void);

// Set optional pause/resume hooks. pause is called just before
// bb_http_client_get_stream; resume is called immediately after (success or
// failure). Pass NULL for either to disable. Returns BB_ERR_INVALID_STATE if
// called before bb_update_check_init.
bb_err_t bb_update_check_set_hooks(bb_update_check_hook_fn pause,
                                   bb_update_check_hook_fn resume);

// Trigger an immediate non-blocking check.
bb_err_t bb_update_check_now(void);

// Copy the latest status snapshot into out. BB_ERR_INVALID_ARG if out is NULL,
// BB_ERR_INVALID_STATE if init hasn't run.
bb_err_t bb_update_check_get_status(bb_update_check_status_t *out);

#ifdef __cplusplus
}
#endif

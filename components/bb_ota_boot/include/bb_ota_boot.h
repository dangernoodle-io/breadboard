#pragma once

#include <stdbool.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bb_ota_boot — OTA-only boot mode for tight / serial-less single-core boards.
 *
 * In-place pull can't clear the TLS+OTA buffers under a fragmented runtime heap
 * (e.g. esp32-s2-mini). Boot mode does the pull at FULL heap: a one-shot NVS
 * flag is armed (via POST /api/update/boot), the device reboots, and before any
 * subsystem starts it resolves the latest release and pulls it, then reboots
 * into the new image.
 *
 * Runtime cost on a tight board is a single lean route (gated by
 * CONFIG_BB_OTA_BOOT_AUTOREGISTER) — no download worker until boot. The manifest
 * resolve + download run in boot mode, so the runtime can leave
 * CONFIG_BB_UPDATE_CHECK_AUTOREGISTER and CONFIG_BB_OTA_PULL_AUTOREGISTER off.
 */

/*
 * Optional progress callback (shared bb_core typedef) for LED/feedback. Fired at
 * START on entry, then the download phase emits PROGRESS(pct) / SUCCESS / FAIL
 * (forwarded to bb_ota_pull). NULL to clear.
 */
void bb_ota_boot_set_progress_cb(bb_ota_progress_cb_t cb);

/* Arm OTA boot mode: persist the one-shot flag so the NEXT boot performs the
 * full-heap pull. Does NOT reboot — the caller (or the HTTP route) decides. */
void bb_ota_boot_arm(void);

/* True if OTA boot mode is armed. */
bool bb_ota_boot_pending(void);

#ifdef ESP_PLATFORM
#include "bb_http.h"

/* Registry hook — registers POST /api/update/boot (arm + reboot). Gated by
 * CONFIG_BB_OTA_BOOT_AUTOREGISTER. */
bb_err_t bb_ota_boot_init(bb_http_handle_t server);

/*
 * Early-boot entry. If armed: clear the flag, bring up link + clock, resolve the
 * latest asset and pull it at full heap (on a fat-stack worker), then reboot
 * into the new image — NEVER returns. If not armed: returns immediately. Call
 * after WiFi STA init, before other subsystems. releases_url/board are the
 * values you would pass to bb_update_check (e.g. ".../releases/latest" and
 * "taipanminer-<board>").
 */
void bb_ota_boot_run_if_pending(const char *releases_url, const char *board);

#endif // ESP_PLATFORM

#ifdef __cplusplus
}
#endif

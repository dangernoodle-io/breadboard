#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bb_ota_boot — OTA-only boot mode for tight / serial-less single-core boards.
 *
 * In-place pull can't clear the TLS+OTA buffers under a fragmented runtime heap
 * (e.g. esp32-s2-mini). Boot mode does the pull at FULL heap: a one-shot NVS
 * flag is armed (via POST /api/update/apply), the device reboots, and before any
 * subsystem starts it resolves the latest release and pulls it, then reboots
 * into the new image.
 *
 * On a boot-mode board bb_ota_boot is the single owner of POST /api/update/apply
 * (bb_ota_pull does not register it — see the BB_OTA_STRATEGY Kconfig choice).
 * The route returns {"status":"rebooting_for_boot_mode_ota"}, distinct from
 * pull's "update_started", so clients wait for the device to reappear on a
 * bumped version rather than poll /api/update/progress.
 *
 * Runtime cost on a tight board is a single lean route (gated by
 * CONFIG_BB_OTA_BOOT_AUTOREGISTER) — no download worker until boot. The manifest
 * resolve + download run in boot mode, so the runtime can leave
 * CONFIG_BB_OTA_CHECK_AUTOREGISTER and CONFIG_BB_OTA_PULL_AUTOREGISTER off.
 */

/*
 * Map an OTA phase to its JSON state string used by GET /api/update/progress.
 * START/PROGRESS -> "downloading", SUCCESS -> "complete", FAIL -> "error".
 * Pure function — safe to call on any platform, useful for host tests.
 */
const char *bb_ota_boot_phase_str(bb_ota_phase_t phase);

/*
 * Set the mDNS service identity for advertise-only mDNS during boot-mode OTA.
 * Only meaningful when CONFIG_BB_OTA_BOOT_PROGRESS_HTTP is enabled; on other
 * configurations this is a no-op that returns BB_OK.
 *
 * Call before bb_ota_boot_run_if_pending(). All strings are copied into
 * fixed-size buffers (max 63 chars each for hostname/service_type/proto; port
 * is stored as-is). Returns BB_ERR_INVALID_ARG if any string exceeds its limit
 * or if hostname/service_type/proto is NULL.
 *
 * hostname     — mDNS hostname (e.g. "taipanminer-s2")
 * service_type — mDNS service type without leading underscore (e.g. "_taipanminer")
 * proto        — transport protocol (e.g. "_tcp")
 * port         — service port (e.g. 80)
 */
bb_err_t bb_ota_boot_set_mdns_service(const char *hostname,
                                      const char *service_type,
                                      const char *proto,
                                      uint16_t    port);

/* Arm OTA boot mode: persist the one-shot flag so the NEXT boot performs the
 * full-heap pull. Does NOT reboot — the caller (or the HTTP route) decides. */
void bb_ota_boot_arm(void);

/* True if OTA boot mode is armed. */
bool bb_ota_boot_pending(void);

#ifdef ESP_PLATFORM
#include "bb_http.h"

/* Registry hook — registers POST /api/update/apply (arm + reboot). Gated by
 * CONFIG_BB_OTA_BOOT_AUTOREGISTER (defaults on only when BB_OTA_STRATEGY_BOOT). */
bb_err_t bb_ota_boot_init(bb_http_handle_t server);

/*
 * Early-boot entry. If armed: clear the flag, bring up link + clock, resolve the
 * latest asset and pull it at full heap (on a fat-stack worker), then reboot
 * into the new image — NEVER returns. If not armed: returns immediately. Call
 * after WiFi STA init, before other subsystems. releases_url/board are the
 * values you would pass to bb_ota_check (e.g. ".../releases/latest" and
 * "taipanminer-<board>").
 */
void bb_ota_boot_run_if_pending(const char *releases_url, const char *board);

#endif // ESP_PLATFORM

#ifdef __cplusplus
}
#endif

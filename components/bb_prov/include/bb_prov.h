#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Parse result codes for provisioning form body
typedef enum {
    BB_PROV_PARSE_OK = 0,
    BB_PROV_PARSE_EMPTY_BODY,
    BB_PROV_PARSE_SSID_REQUIRED,
} bb_prov_parse_result_t;

// Parse a URL-encoded provisioning POST body into ssid/pass.
// body_len <= 0 → BB_PROV_PARSE_EMPTY_BODY.
// Missing/empty ssid → BB_PROV_PARSE_SSID_REQUIRED.
// body need not be null-terminated; function treats body_len as authoritative.
bb_prov_parse_result_t bb_prov_parse_body(
    const char *body, int body_len,
    char *ssid_out, size_t ssid_size,
    char *pass_out, size_t pass_size);

// ============================================================================
// ESP-IDF-SPECIFIC API — provisioning lifecycle
// ============================================================================

#ifdef ESP_PLATFORM
#include "bb_http_server.h"
#include <stdint.h>

// AP mode (SoftAP + captive DNS) has moved to bb_wifi_ap (KB 781) — the
// pure AP primitive, zero HTTP: bb_wifi_ap_start()/bb_wifi_ap_stop()/
// bb_wifi_ap_get_ssid()/bb_wifi_ap_set_ssid_prefix()/
// bb_wifi_ap_set_password() (see components/bb_wifi_ap/include/bb_wifi_ap.h).
// This is extraction only — bb_prov does not call into bb_wifi_ap; it no
// longer owns or duplicates this API. Callers (or the future bb_wifi_prov
// lifecycle FSM) invoke bb_wifi_ap_start()/stop() themselves alongside
// bb_prov_start()/stop().

// Provisioning synchronization
/**
 * Block until provisioning completes.
 * @param timeout_ms  How long to wait in ms; UINT32_MAX = wait forever.
 * @return true if provisioning completed, false on timeout.
 */
bool bb_prov_wait_done(uint32_t timeout_ms);

/** Signal provisioning complete. Called by http_server's /save handler. */
void bb_prov_signal_done(void);

// Optional /save callback. Invoked after bb_prov parses+saves wifi creds.
// Consumer parses any additional form fields from body and writes the HTTP response.
// If not set, bb_prov sends 204 No Content. bb_prov_signal_done() is called after.
typedef bb_err_t (*bb_prov_save_cb_t)(bb_http_request_t *req, const char *body, int len);
void bb_prov_set_save_callback(bb_prov_save_cb_t cb);

/**
 * Optional callback invoked during bb_prov_start to let the consumer register
 * extra dynamic GET routes (e.g. /api/hardware, /api/pool-test) that the
 * provisioning UI needs. Called after common routes and before the captive-
 * portal wildcard, so these routes win first-match.
 *
 * Pass NULL when the prov UI only needs the built-in common routes
 * (/api/version, /api/scan, /api/reboot).
 */
typedef bb_err_t (*bb_prov_extra_routes_fn_t)(bb_http_handle_t server);

/**
 * Start HTTP server in provisioning mode.
 *
 * Registers (in order): POST /save, consumer assets, common routes
 * (/api/version, /api/scan, /api/reboot), @p extra consumer routes if
 * non-NULL, and finally the captive-portal wildcard GET (matches all URIs).
 *
 * Caller MUST supply at least one asset with path="/" — no default form is
 * provided. For bare-minimum bringup, add REQUIRES bb_prov_default_form to
 * your component and pass:
 *   const bb_http_asset_t *a = bb_prov_default_form_get();
 *   bb_prov_start(a, 1, NULL);
 *
 * @param assets  Array of static HTTP assets; must contain a path="/" entry.
 * @param n       Number of entries in @p assets.
 * @param extra   Optional callback for consumer-specific dynamic routes.
 */
bb_err_t bb_prov_start(const bb_http_asset_t *assets, size_t n,
                       bb_prov_extra_routes_fn_t extra);

// Stop provisioning mode: unregister POST /save, OPTIONS /*, GET /* captive-portal wildcard,
// and any assets registered via bb_prov_start. Caller is responsible for registering app
// routes afterward.
void bb_prov_stop(void);

#endif

#ifdef __cplusplus
}
#endif

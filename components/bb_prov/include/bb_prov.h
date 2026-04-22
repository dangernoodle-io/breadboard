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
#include "bb_nv.h"
#include "bb_http.h"
#include <stdint.h>

// AP mode — for provisioning
bb_err_t bb_prov_start_ap(void);        // starts AP + captive DNS
void bb_prov_stop_ap(void);              // stops AP + DNS, deinits wifi
void bb_prov_get_ap_ssid(char *buf, size_t len);  // get AP SSID

// Set AP SSID prefix (e.g. "TaipanMiner-"). Must be called before bb_prov_start_ap().
// Defaults to "BB-" if not set.
void bb_prov_set_ap_ssid_prefix(const char *prefix);

// Set AP WPA2 password. Must be called before bb_prov_start_ap().
// Defaults to "breadboard" if not set. Passing NULL restores the default.
// WPA2 requires 8–63 chars; the caller is responsible for validity.
void bb_prov_set_ap_password(const char *password);

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
 * Start HTTP server in provisioning mode.
 *
 * Registers POST /save and a captive-portal wildcard GET. Registers each
 * entry in @p assets as a static GET route via bb_http_register_assets.
 *
 * Caller MUST supply at least one asset with path="/" — no default form is
 * provided. For bare-minimum bringup, add REQUIRES bb_prov_default_form to
 * your component and pass:
 *   const bb_http_asset_t *a = bb_prov_default_form_get();
 *   bb_prov_start(a, 1);
 *
 * @param assets  Array of static HTTP assets; must contain a path="/" entry.
 * @param n       Number of entries in @p assets.
 */
bb_err_t bb_prov_start(const bb_http_asset_t *assets, size_t n);

// Stop provisioning mode: unregister POST /save, OPTIONS /*, GET /* captive-portal wildcard,
// and any assets registered via bb_prov_start. Caller is responsible for registering app
// routes afterward.
void bb_prov_stop(void);

#endif

#ifdef __cplusplus
}
#endif

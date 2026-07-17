#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief bb_wifi_ap — SoftAP + captive-DNS primitive (KB 781): pure AP
 * bring-up and a captive DNS responder, zero HTTP. Extracted from bb_prov's
 * former AP code; bb_prov does not call bb_wifi_ap_start()/stop(). It is a
 * standalone primitive — callers (or the future bb_wifi_prov lifecycle FSM)
 * invoke bb_wifi_ap_start()/stop() themselves; nothing wires it into
 * bb_prov automatically. The AP<->STA lifecycle FSM, net-event topics, and
 * recovery model are out of scope here — see bb_wifi_prov for provisioning
 * orchestration.
 */

// ---------------------------------------------------------------------------
// Portable core — host + ESP-IDF, no platform dependency.
// ---------------------------------------------------------------------------

// Build the AP SSID "<prefix><last two MAC bytes, uppercase hex>" into out.
// Returns BB_ERR_INVALID_ARG on a NULL prefix/mac/out, out_size == 0, or if
// the formatted SSID (incl. terminator) would not fit in out_size.
bb_err_t bb_wifi_ap_build_ssid(const char *prefix, const uint8_t mac[6],
                                char *out, size_t out_size);

// Normalize an AP SSID prefix: copies prefix into out, or clears out to an
// empty string when prefix is NULL (matching bb_wifi_ap_set_ssid_prefix's
// "NULL restores the default" contract — the default itself is applied by
// the caller, not baked in here). No-op if out is NULL or out_size == 0.
void bb_wifi_ap_normalize_prefix(const char *prefix, char *out, size_t out_size);

// Normalize an AP WPA2 password: copies password into out, or "breadboard"
// when password is NULL. No-op if out is NULL or out_size == 0.
void bb_wifi_ap_normalize_password(const char *password, char *out, size_t out_size);

// Build a captive-DNS UDP response for a single query in req[0..req_len-1],
// answering every query with an A record pointing at answer_ip (4 bytes,
// network order e.g. {192,168,4,1}). Writes into out (out_cap bytes) and
// returns the response length in bytes. req_len must be in [12, 512]
// (12 = minimum DNS header size; 512 = the classic UDP DNS message ceiling).
// out_cap must be at least req_len + 16 (the copied question plus the fixed
// 16-byte answer trailer) for the response to fit.
// Returns 0 (no response written) if req/answer_ip/out is NULL, req_len is
// outside [12, 512], or the response would not fit in out_cap.
int bb_wifi_ap_dns_build_response(const uint8_t *req, int req_len,
                                   const uint8_t answer_ip[4],
                                   uint8_t *out, int out_cap);

// ---------------------------------------------------------------------------
// ESP-IDF SoftAP lifecycle
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM

// Start the SoftAP + captive DNS responder. OR-arbitrates WIFI_MODE_AP into
// the esp_wifi driver's current mode (via esp_wifi_get_mode) so bringing up
// the AP does not clobber an already-connected STA -- APSTA if STA is
// active, AP-only otherwise. Idempotent: safe to call from bb_prov's
// provisioning-mode entry.
bb_err_t bb_wifi_ap_start(void);

// Stop the SoftAP + DNS responder. Restores STA-only mode if a STA was
// active when bb_wifi_ap_start ran; otherwise stops and deinits the
// esp_wifi driver entirely.
void bb_wifi_ap_stop(void);

// Get the AP SSID chosen at bb_wifi_ap_start time.
void bb_wifi_ap_get_ssid(char *buf, size_t len);

// Set the AP SSID prefix (e.g. "TaipanMiner-"). Must be called before
// bb_wifi_ap_start(). Defaults to "BB-" if never set; passing NULL restores
// that default.
void bb_wifi_ap_set_ssid_prefix(const char *prefix);

// Set the AP WPA2 password. Must be called before bb_wifi_ap_start().
// Defaults to "breadboard" if never set. Passing NULL restores the default.
// WPA2 requires 8-63 chars; the caller is responsible for validity.
void bb_wifi_ap_set_password(const char *password);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif

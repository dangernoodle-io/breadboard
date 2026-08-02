#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief bb_wifi_prov — Wi-Fi provisioning HTTP routes: parses a POSTed
 * SSID/password form and a captive-portal redirect. Registers POST /save,
 * POST /api/wifi/scan (B1-809 -- this component's own SSID-list endpoint;
 * see bb_wifi_prov_start()'s doc; CAUTION -- its "refresh" query param hops
 * the radio channel and can transiently drop the requesting client's own
 * SoftAP association), and a shared GET /* wildcard on the shared HTTP
 * server that serves consumer assets plus the captive-portal redirect as
 * its no-match fallback; does not register /api/version or /api/diag/reboot
 * (those live in bb_wifi_http / bb_system), and does not itself bring up
 * SoftAP or drive a Wi-Fi lifecycle state machine (see bb_wifi_ap for AP
 * bring-up).
 */

// Parse result codes for provisioning form body
typedef enum {
    BB_WIFI_PROV_PARSE_OK = 0,
    BB_WIFI_PROV_PARSE_EMPTY_BODY,
    BB_WIFI_PROV_PARSE_SSID_REQUIRED,
} bb_wifi_prov_parse_result_t;

// Parse a URL-encoded provisioning POST body into ssid/pass/hostname.
// body_len <= 0 → BB_WIFI_PROV_PARSE_EMPTY_BODY.
// Missing/empty ssid → BB_WIFI_PROV_PARSE_SSID_REQUIRED.
// body need not be null-terminated; function treats body_len as authoritative.
// hostname is OPTIONAL: absent or empty is never an error and never changes
// the OK/EMPTY_BODY/SSID_REQUIRED result -- hostname_out is simply left as an
// empty string. This function does NOT validate the hostname (charset/
// RFC-1123 shape/length) -- that's bb_settings_hostname_set()'s job; the
// caller decides what to do with an invalid value.
bb_wifi_prov_parse_result_t bb_wifi_prov_parse_body(
    const char *body, int body_len,
    char *ssid_out, size_t ssid_size,
    char *pass_out, size_t pass_size,
    char *hostname_out, size_t hostname_size);

// ============================================================================
// Save-confirmation page (host-testable, pure) — B1-809 no-callback /save
// response. Escaping is required because ssid/hostname are attacker-
// controlled in the general case (they're whatever the client just POSTed).
// ============================================================================

// HTML-escape src into out (& < > " '), truncating (never mid-entity —
// an entity is only ever emitted if it fits whole) rather than overflowing.
// out is always NUL-terminated when out_size > 0. Returns the number of
// bytes written to out, excluding the NUL terminator. NULL src is treated
// as "" (writes just the terminator, if room). NULL out or out_size == 0
// is a no-op returning 0.
size_t bb_wifi_prov_html_escape(const char *src, char *out, size_t out_size);

// Worst-case single-field escape growth this component budgets for: every
// byte of the largest form field (hostname, BB_MDNS_HOSTNAME_MAX-class, 33
// bytes) expanding to the longest entity ("&quot;"/"&#39;", 6 bytes) plus
// the NUL terminator. SSID (32 bytes) fits the same budget.
#define BB_WIFI_PROV_HTML_ESCAPED_MAX 200

// Render the no-callback /save success page: confirms the credentials were
// saved, names the network (and hostname, if one was set) the user just
// typed, warns that this SoftAP is about to disappear (expected, not an
// error), states that the device will finish setting up and reconnect to
// that network, and states where to find it afterward (its mDNS name, if
// set). Deliberately neutral about HOW it finishes setting up (a consumer
// may or may not restart to get there) — this component never promises a
// restart it cannot guarantee; a restarting consumer's copy still reads
// true. ssid/hostname are HTML-escaped internally — pass the raw (already
// URL-decoded, not yet escaped) values straight from bb_wifi_prov_parse_body.
// hostname may be "" (omits the mDNS/hostname-specific lines). Self-
// contained: no external resources, no script. Truncates safely (always
// NUL-terminates within out_size) rather than overflowing; a legitimate
// ssid(32)/hostname(33) pair fits comfortably within
// BB_WIFI_PROV_SAVED_PAGE_MAX. Returns the number of bytes written to out,
// excluding the NUL terminator. NULL out or out_size == 0 is a no-op
// returning 0.
#define BB_WIFI_PROV_SAVED_PAGE_MAX 1536
size_t bb_wifi_prov_render_saved_page(char *out, size_t out_size,
                                      const char *ssid, const char *hostname);

// MEDIUM review fix (B1-809): the confirmation page must never claim a
// hostname that bb_settings_hostname_set() actually rejected -- pass its
// caller-supplied hostname through here first, with hostname_saved set to
// whether that call succeeded. Returns hostname unchanged when
// hostname_saved is true, otherwise "" (bb_wifi_prov_render_saved_page()
// already treats "" as "no hostname was set"). Pure/host-testable.
const char *bb_wifi_prov_saved_page_hostname(const char *hostname, bool hostname_saved);

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
// This is extraction only — bb_wifi_prov does not call into bb_wifi_ap; it no
// longer owns or duplicates this API. Callers (or the future bb_wifi_prov
// lifecycle FSM) invoke bb_wifi_ap_start()/stop() themselves alongside
// bb_wifi_prov_start()/stop().

// Provisioning synchronization
/**
 * Block until provisioning completes.
 * @param timeout_ms  How long to wait in ms; UINT32_MAX = wait forever.
 * @return true if provisioning completed, false on timeout.
 */
bool bb_wifi_prov_wait_done(uint32_t timeout_ms);

/** Signal provisioning complete. Called by http_server's /save handler. */
void bb_wifi_prov_signal_done(void);

// Optional /save callback. Invoked after bb_wifi_prov parses+saves wifi creds.
// Consumer parses any additional form fields from body and writes the HTTP response.
// If not set, bb_wifi_prov renders a minimal HTML confirmation page instead
// (see bb_wifi_prov_render_saved_page()) rather than 204 No Content -- a
// bodyless 204 leaves the browser nothing to render just before this AP
// tears down, which reads as a failure even though the save succeeded.
// bb_wifi_prov_signal_done() is called after, once the response is written.
typedef bb_err_t (*bb_wifi_prov_save_cb_t)(bb_http_request_t *req, const char *body, int len);
void bb_wifi_prov_set_save_callback(bb_wifi_prov_save_cb_t cb);

/**
 * Optional callback invoked during bb_wifi_prov_start to let the consumer register
 * extra dynamic GET routes (e.g. /api/hardware, /api/pool-test) that the
 * provisioning UI needs. Called after POST /save and before the captive-
 * portal wildcard, so these routes win first-match.
 *
 * Pass NULL when the prov UI needs no extra routes beyond POST /save,
 * POST /api/wifi/scan, and the captive-portal wildcard. bb_wifi_prov itself
 * does not register /api/version or /api/diag/reboot — those live in
 * bb_wifi_http / bb_system; a consumer wanting them wires them in via this
 * callback or its own codegen/handwire composition.
 */
typedef bb_err_t (*bb_wifi_prov_extra_routes_fn_t)(bb_http_handle_t server);

/**
 * Start HTTP server in provisioning mode.
 *
 * Registers (in order): POST /save, POST /api/wifi/scan (this component's
 * own scan endpoint -- B1-809; serves bb_wifi_scan_get_cached() by default,
 * an explicit "refresh" query param triggers a background rescan; see
 * bb_wifi_prov.c's prov_scan_handler for the full contract), @p extra
 * consumer routes if non-NULL, and finally consumer assets plus the
 * captive-portal redirect together as a single GET /* wildcard (the
 * redirect fires only on no-match). No other routes are registered by this
 * component.
 *
 * CAUTION -- POST /api/wifi/scan?refresh: the rescan it triggers hops the
 * radio channel, which transiently drops SoftAP-associated clients,
 * including whichever client just issued the "refresh" request. The HTTP
 * response is written before that disruption in practice (the rescan is a
 * detached low-priority task, not run inline), but that ordering is
 * scheduler-cooperative, not guaranteed -- a portal UI using "refresh"
 * should expect to retry/reconnect rather than treat a dropped connection
 * as a failure.
 *
 * POST /api/wifi/scan tolerates already being served by another composed
 * component (e.g. bb_wifi_http, first-registration-wins) -- see
 * route_registry.c's bb_dispatch_api_add() -- and never fails
 * bb_wifi_prov_start() over that.
 *
 * Caller MUST supply at least one asset with path="/" — bb_wifi_prov ships a
 * built-in default form but never registers it itself (composition-only).
 * For bare-minimum bringup, include "bb_wifi_prov_default_form.h" and pass:
 *   const bb_http_asset_t *a = bb_wifi_prov_default_form_get();
 *   bb_wifi_prov_start(a, 1, NULL);
 *
 * Also activates the provisioning route gate (B1-1279 PR-4): wires
 * bb_wifi_prov_is_active() as bb_http_prov_gate's active-fn and allowlists
 * GET /ping, POST /save, POST /api/wifi/scan, and a GET entry per @p assets
 * path -- everything else served by the shared HTTP server 404s while
 * provisioning is active. @p extra's routes are NOT auto-allowlisted; a
 * consumer whose custom UI genuinely needs one reachable during
 * provisioning must call bb_http_prov_allow() for it itself. See
 * wifi_prov_gate_wire.h for the full rationale.
 *
 * If this composition's fixed(3)+n allowlist entries exceed
 * BB_HTTP_PROV_ALLOWLIST_CAP, this call still returns BB_OK (a dropped
 * asset fails closed, it does not abort provisioning) -- see
 * bb_wifi_prov_gate_degraded() to detect and report that condition.
 *
 * @param assets  Array of static HTTP assets; must contain a path="/" entry.
 * @param n       Number of entries in @p assets.
 * @param extra   Optional callback for consumer-specific dynamic routes.
 */
bb_err_t bb_wifi_prov_start(const bb_http_asset_t *assets, size_t n,
                       bb_wifi_prov_extra_routes_fn_t extra);

// Stop provisioning mode: unregister POST /save, OPTIONS /<path>, and the single shared
// GET /* wildcard handler that serves both consumer assets registered via bb_wifi_prov_start
// and the captive-portal redirect fallback. Caller is responsible for registering app
// routes afterward.
void bb_wifi_prov_stop(void);

// ============================================================================
// ESP-IDF-SPECIFIC API — boot-time / user-requested provisioning entry
// (B1-809 PR4)
// ============================================================================

/**
 * Boot-time provisioning entry point. Reads bb_settings_wifi_has_creds()
 * and bb_settings_wifi_provisioned_get() and, per the ONE-bit entry policy
 * documented on wifi_prov_policy.h's wifi_prov_entry_decision() (component-
 * private; see that header for the full rationale), starts the
 * provisioning manager and posts EV_ENTRY_FIRST_BOOT or
 * EV_ENTRY_DEPROV_ANOMALY ONLY when no WiFi credentials are present. The
 * `provisioned` flag never decides whether a portal opens — only which
 * reason is reported — so a board with valid committed creds is never
 * stranded, whether or not it has completed a validated connect yet.
 *
 * Safe to call when creds already exist: does nothing but return BB_OK.
 * Deliberately does NOT start the provisioning manager task in that case —
 * a board with valid creds has no need for it (normal STA connect is owned
 * by wifi_reconn, not this component). One consequence: a later
 * bb_wifi_prov_request_portal() call is a no-op unless THIS call already
 * started the manager (i.e. unless creds were absent at boot) — a future
 * consumer wanting a "reprovision" trigger on an already-provisioned board
 * needs its own path to start the manager; out of scope here.
 *
 * @param assets Array of static HTTP assets for the provisioning UI — see
 *   bb_wifi_prov_start()'s doc (must contain a path="/" entry).
 * @param n Number of entries in @p assets.
 * @param extra Optional callback for consumer-specific dynamic routes — see
 *   bb_wifi_prov_start()'s doc. Pass NULL if not needed.
 *
 * Not safe to call concurrently with itself — the provisioning manager's
 * init guard is a plain check with no lock, so call this once, from a
 * single task, typically at boot.
 */
bb_err_t bb_wifi_prov_autoinit(const bb_http_asset_t *assets, size_t n,
                                bb_wifi_prov_extra_routes_fn_t extra);

/**
 * User-requested (re-)provisioning entry point: posts EV_ENTRY_USER_REQUESTED
 * to the provisioning manager. Ships as a callable API ONLY — bb_wifi_prov
 * registers no route, button, or other trigger that calls this; wiring a
 * trigger (an HTTP route, a GPIO button, etc.) is the consumer's concern.
 *
 * No-op if the provisioning manager isn't running — either because
 * bb_wifi_prov_autoinit() was never called, or because it was called while
 * creds were already present (see its doc for why that path deliberately
 * does not start the manager).
 */
void bb_wifi_prov_request_portal(void);

/**
 * "Provisioning active" signal (B1-1279 PR2). True from the moment the
 * captive portal comes up (SoftAP + provisioning HTTP routes registered)
 * until it is fully torn down, INCLUDING the settle window after a
 * successful POST /save where credentials are already committed but the
 * portal routes are still live while the in-flight HTTP response finishes
 * flushing.
 *
 * Deliberately NOT equivalent to "credentials are absent" (has_creds):
 * POST /save commits credentials via bb_settings_wifi_set() and only THEN
 * starts that settle window before the portal actually closes -- so there
 * is a window where credentials exist AND the captive portal is still live
 * and serving. A caller gating on has_creds instead of this signal would
 * treat that window as "not provisioning" and open its full API while an
 * unauthenticated client may still be associated to the SoftAP. Use this
 * accessor for any such gate, not a has_creds/provisioned check.
 *
 * Returns false if bb_wifi_prov_autoinit() was never called, or was called
 * while credentials were already present (which deliberately never starts
 * the provisioning manager -- see bb_wifi_prov_autoinit()'s doc).
 *
 * Non-blocking; safe to call from any task, including concurrently with the
 * provisioning manager's own task.
 *
 * Wired as bb_http_prov_gate's active-fn (bb_http_prov_gate_set_active_fn())
 * by bb_wifi_prov_start() itself (B1-1279 PR-4) -- this is what makes the
 * default-deny provisioning route gate (components/bb_http_server/include/
 * bb_http_prov_gate.h) actually gate anything in production.
 */
bool bb_wifi_prov_is_active(void);

/**
 * Review fix (B1-1279 PR-4 finding): was the provisioning route gate's
 * allowlist fully registered? False means bb_wifi_prov_start()'s composition
 * (fixed(3) required routes + n caller-supplied asset paths) exceeded
 * BB_HTTP_PROV_ALLOWLIST_CAP (bb_http_prov_gate.h) and one or more entries
 * were dropped -- typically a custom-branded portal shipping more assets
 * than the cap allows. The gate still fails CLOSED on whatever didn't fit
 * (denied, not leaked) -- this is a portal-breakage signal (the dropped
 * asset(s) 404 for the life of the process), not a security regression.
 *
 * bb_wifi_prov_start() deliberately still returns BB_OK in this case rather
 * than aborting provisioning -- a branded-portal asset-count mistake is a
 * build-time composition bug, and failing the whole provisioning flow over
 * it would leave the device unprovisionable rather than merely
 * portal-degraded. Callers that want to surface this loudly (diagnostics,
 * an operator-visible health check, a log-and-reboot policy, etc.) must poll
 * this accessor themselves -- bb_wifi_prov_start() only logs it once.
 *
 * Returns false before bb_wifi_prov_start() has ever been called. Sticky
 * for the life of the process once true: the allowlist is wired at most
 * once (see s_gate_wired in bb_wifi_prov.c), so a later stop/start cycle
 * cannot retry or clear a degraded state -- only a reboot with a corrected
 * asset composition can.
 */
bool bb_wifi_prov_gate_degraded(void);

#endif

#ifdef __cplusplus
}
#endif

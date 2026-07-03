#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "bb_core.h"
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// Shared "wifi" cache/telemetry topic constant. Referenced by bb_wifi_routes
// (espidf) and bb_pub_wifi (host+espidf) — both already depend on this
// header — so the bb_cache tag, bb_pub subtopic, and openapi schema name
// cannot drift independently. Not the same topic as "net.health"
// (bb_net_health) — that name is out of scope for this constant.
#define BB_TOPIC_WIFI "wifi"

// ---------------------------------------------------------------------------
// Public types — usable on every backend.
// ---------------------------------------------------------------------------

#define WIFI_SCAN_MAX 20

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;   // true if not WIFI_AUTH_OPEN
} bb_wifi_ap_t;

// Snapshot of the current STA connection. Populated by bb_wifi_get_info.
// On backends that don't surface a given field, it is zeroed.
typedef struct {
    char ssid[33];       // SSID of associated AP, empty if not connected
    uint8_t bssid[6];    // BSSID of associated AP
    int8_t rssi;         // signal strength, 0 if not connected
    char ip[16];         // dotted-quad IPv4, "0.0.0.0" if no IP
    bool connected;      // true iff has_ip
    uint8_t disc_reason; // last disconnect reason code
    uint32_t disc_age_s; // seconds since last disconnect, 0 if never
    int retry_count;     // STA retry attempts since last connect
} bb_wifi_info_t;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Idempotent one-shot bring-up of the underlying network stack. On ESP-IDF
// this initializes esp_netif and the default event loop. On backends where
// the platform handles this implicitly (Arduino), this is a BB_OK no-op so
// portable consumer code can call it unconditionally.
bb_err_t bb_wifi_ensure_netif(void);

// STA mode connect. bb_wifi_init restarts the system on connect timeout
// (intended for normal boot); bb_wifi_init_sta returns an error on timeout
// instead (intended for provisioning retry loops). Both block until
// connected or timeout.
bb_err_t bb_wifi_init(void);
bb_err_t bb_wifi_init_sta(void);

// Force WiFi reassociation — recover from a zombie-connected state.
// On backends without explicit reassociation control (Arduino), this is
// a BB_OK no-op.
void bb_wifi_force_reassociate(void);

// Stop and restart the WiFi STA driver to clear wedged driver state.
// Re-applies the saved STA config and inactive-time. Calls esp_wifi_connect()
// explicitly; safe to call from the reconn task context.
// Only available on ESP-IDF platform.
void bb_wifi_restart_sta(void);

// App-driven WiFi recovery request.
// Returns BB_OK immediately (non-blocking). If the STA already has no IP,
// this is a no-op (BB_OK) — the FSM already owns recovery.
// Debounced: at most one action per CONFIG_BB_WIFI_RECOVERY_COOLDOWN_S.
// When triggered, signals the reconn task to call bb_wifi_restart_sta().
// Bumps bb_wifi_get_egress_dead_count() and logs reason (never silent).
// Always compiled; safe to call from any task context.
bb_err_t bb_wifi_request_recovery(const char *reason);

// ---------------------------------------------------------------------------
// Hostname
// ---------------------------------------------------------------------------

// Set the DHCP host name (Option 12) advertised by the STA netif.
// Independent of mDNS — call bb_mdns_set_hostname() separately if you
// want the same value on both surfaces. Safe to call before STA connects;
// the value is sent on the next DHCP DISCOVER/REQUEST. Returns
// BB_ERR_INVALID_ARG on NULL/empty hostname; BB_ERR_INVALID_STATE if
// the STA netif isn't initialized yet. On backends without hostname
// support (e.g. CC3000), returns BB_OK no-op.
bb_err_t bb_wifi_set_hostname(const char *hostname);

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------
// On backends without scan support, the blocking variant returns 0 and the
// async pair is a BB_OK / 0 no-op — callers can invoke them unconditionally.

// Blocking WiFi scan. Returns number of APs found (up to max_results).
int bb_wifi_scan_networks(bb_wifi_ap_t *results, int max_results);

// Start a non-blocking WiFi scan in the background. Returns immediately.
void bb_wifi_scan_start_async(void);

// Get cached WiFi scan results. Returns number of APs (0 if no scan done).
int bb_wifi_scan_get_cached(bb_wifi_ap_t *results, int max_results);

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
// Backends that lack a native event loop (Arduino) dispatch callbacks
// synchronously from inside bb_wifi_init_sta and bb_http_server_poll
// transitions, but the contract is unchanged from the consumer's POV.

typedef void (*bb_wifi_on_got_ip_cb_t)(void);
void bb_wifi_register_on_got_ip(bb_wifi_on_got_ip_cb_t cb);

typedef void (*bb_wifi_on_disconnect_cb_t)(void);
void bb_wifi_register_on_disconnect(bb_wifi_on_disconnect_cb_t cb);

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void bb_wifi_get_disconnect(uint8_t *reason, int64_t *age_us);
int  bb_wifi_get_retry_count(void);
bb_err_t bb_wifi_get_ip_str(char *out, size_t out_len);
bb_err_t bb_wifi_get_rssi(int8_t *out);
bool bb_wifi_has_ip(void);

// True iff the STA is L2-associated to an AP (regardless of IP state).
// Boot-safe: wraps esp_wifi_sta_get_ap_info() == ESP_OK, the same check the
// no-IP watchdog in wifi_reconn.c uses — safe to call before the wifi driver
// is fully up (returns false, not an error). Host stub is test-injectable
// via bb_wifi_test_set_associated() (BB_WIFI_TESTING).
bool bb_wifi_is_associated(void);

// Populate out with a snapshot of the current STA state. Fields the
// backend cannot supply are zeroed. Returns BB_ERR_INVALID_ARG on null.
bb_err_t bb_wifi_get_info(bb_wifi_info_t *out);

// Lost-IP-while-associated recovery counters.
// Return 0 if the reconnect manager is not active or no lost-IP has occurred.
uint32_t bb_wifi_get_lost_ip_count(void);
uint32_t bb_wifi_get_lost_ip_age_s(void);

// Egress-dead recovery counter: times the active gateway-probe found the
// gateway unreachable and triggered bb_wifi_restart_sta. Returns 0 if the
// reconnect manager is not active or no egress-dead event has occurred.
uint32_t bb_wifi_get_egress_dead_count(void);

// No-IP-while-associated watchdog recovery counter: times the ST_IDLE watchdog
// detected associated-but-no-IP zombie state and triggered bb_wifi_restart_sta.
// Returns 0 if the reconnect manager is not active or no no-IP event has occurred.
uint32_t bb_wifi_get_no_ip_count(void);

// Times bb_wifi_restart_sta() has been called as part of any WiFi recovery path.
// Incremented inside bb_wifi_restart_sta() only — callers must NOT increment it.
// Returns 0 if bb_wifi_restart_sta() has never been invoked.
uint32_t bb_wifi_get_restart_sta_count(void);

// Roam / BSSID-change counter (B1-497) — OBSERVE-ONLY telemetry, wired to NO
// recovery action. Incremented in the STA_CONNECTED event handler when the
// newly-associated BSSID differs from the previously cached one (a roam
// within the same SSID, or reassociation to a different AP). The first
// connect since boot (no prior cached BSSID) does NOT count as a roam.
// Returns 0 if no roam has occurred since boot.
uint32_t bb_wifi_get_roam_count(void);

// Seconds since the most recent roam event (see bb_wifi_get_roam_count).
// Returns 0 if no roam has occurred since boot (sentinel — matches
// bb_wifi_get_lost_ip_age_s()).
uint32_t bb_wifi_get_roam_age_s(void);

// Pure roam-detection predicate (B1-497): true iff prior_bssid is non-zero
// (i.e. this is not the first connect since boot) and differs from
// new_bssid. Host-testable; the sole production caller is the ESP-IDF
// STA_CONNECTED handler in bb_wifi.c. Safe to call with NULL pointers
// (returns false).
bool bb_wifi_is_roam(const uint8_t prior_bssid[6], const uint8_t new_bssid[6]);

// RSSI at the moment of the most recent WIFI_EVENT_STA_DISCONNECTED event.
// Captured from the periodically-refreshed RSSI cache (s_cached_rssi) before
// the AP record is torn down, avoiding a stale/invalid read inside the handler.
// Returns INT8_MIN if no disconnect has occurred since boot (sentinel = "no reading").
int8_t bb_wifi_get_disconnect_rssi(void);

// Duration in seconds of the STA's most recently ENDED connected session
// (elapsed time between the WIFI_EVENT_STA_CONNECTED that started it and
// the following WIFI_EVENT_STA_DISCONNECTED that ended it). OBSERVE-ONLY
// telemetry — captured at disconnect time, no recovery action attached.
// Returns 0 if no session has ended since boot (sentinel — matches
// bb_wifi_get_lost_ip_age_s()/bb_wifi_get_roam_age_s()). The value is frozen
// at the last disconnect; it does not update live while connected.
uint32_t bb_wifi_get_last_session_s(void);

// Human-readable name for a WiFi disconnect reason code: covers the common
// esp_wifi standard reasons (AUTH_EXPIRE, AUTH_LEAVE,
// DISASSOC_DUE_TO_INACTIVITY, 4WAY_HANDSHAKE_TIMEOUT, BEACON_TIMEOUT,
// NO_AP_FOUND, ASSOC_FAIL, HANDSHAKE_TIMEOUT, CONNECTION_FAIL) plus the three
// breadboard sentinels (BB_WIFI_REASON_BB_LOST_IP/_EGRESS_DEAD/_NO_IP_WATCHDOG).
// Unmapped codes return "other"; reason 0 returns "unknown". Never returns
// NULL. Pure, host-testable, fully reentrant (every branch returns a static
// string literal) — no ESP-IDF dependency.
const char *bb_wifi_disc_reason_str(uint8_t reason);

// Breadboard sentinel disconnect-reason codes injected into the histogram
// returned by bb_wifi_get_reason_histogram. esp_wifi standard reasons occupy
// 1-24/53-67/200-208; these three values are free and fit uint8_t (< 256).
// Do NOT change these numeric values — they are wire-visible in
// reason_histogram (GET /api/diag/net). Single source of truth: the private
// WIFI_REASON_BB_* macros in components/bb_wifi/wifi_reconn_policy.h (the
// production writer) alias these public constants.
#define BB_WIFI_REASON_BB_LOST_IP        99
#define BB_WIFI_REASON_BB_EGRESS_DEAD    100
#define BB_WIFI_REASON_BB_NO_IP_WATCHDOG 101

// Copy the disconnect reason histogram into out[0..len-1].
// Indexes BB_WIFI_REASON_BB_LOST_IP/_EGRESS_DEAD/_NO_IP_WATCHDOG (99/100/101)
// are breadboard sentinels. Standard esp_wifi reasons occupy the remaining
// slots. If the reconnect manager is not active, out is zeroed.
// Safe to call with NULL or len==0 (no-op).
void bb_wifi_get_reason_histogram(uint16_t *out, size_t len);

// Find the top standard (non-sentinel) disconnect reason in a 256-entry
// histogram as returned by bb_wifi_get_reason_histogram. Skips the three
// breadboard sentinel buckets (BB_WIFI_REASON_BB_LOST_IP / _EGRESS_DEAD /
// _NO_IP_WATCHDOG). Sets *out_count to the highest non-sentinel count found
// (0 if all zero, or hist is NULL); returns the bucket index (reason code)
// for that count, or 0 if all are zero. hist must point to at least 256
// entries. Pure, no side effects; safe to call with hist==NULL or
// out_count==NULL.
uint8_t bb_wifi_reason_histogram_top(const uint16_t *hist, uint16_t *out_count);

#ifdef ESP_PLATFORM
// ICMP ping a target IPv4 address. target_addr is a raw IPv4 address in
// esp_ip4_addr byte order (i.e. the same value as esp_ip4_addr_t.addr).
// Sends a single echo-request and waits up to timeout_ms for an echo-reply.
// Sets *out_reachable to true on reply, false on timeout. Always compiled
// (reusable; CI validates the esp_ping symbol). Returns BB_OK or an error.
bb_err_t bb_wifi_ping(uint32_t target_addr, uint32_t timeout_ms,
                      bool *out_reachable);

// Convenience wrapper: pings the current STA default gateway. Returns false
// if no IP/gateway info is available or if the ICMP ping times out.
bool bb_wifi_gateway_reachable(uint32_t timeout_ms);
#endif /* ESP_PLATFORM */

// ---------------------------------------------------------------------------
// Gateway-reachability probe (observe-only, B1-518 PR2)
// ---------------------------------------------------------------------------
// Runs on a dedicated worker task (see platform/espidf/bb_wifi/bb_wifi_gw_probe.c),
// gated by CONFIG_BB_WIFI_GW_PROBE_ENABLE. OBSERVE-ONLY: the worker's policy
// decision is always discarded — this signal never triggers recovery on its
// own (#578). bb_net_health (a later PR) pulls this status for correlation.

// Snapshot of the gateway-probe worker's last observed state. Populated by
// bb_wifi_get_gateway_status.
typedef struct {
    bool     gw_reachable;      // result of the most recent gateway ping
    uint8_t  gw_fail_streak;    // consecutive probe failures (observe-owned state,
                                 // SEPARATE from the live reconnect FSM's streak)
    uint32_t gw_probe_count;    // cumulative probes run since boot
    uint32_t gw_fail_count;     // cumulative probe failures since boot
    uint32_t gw_dead_count;     // cumulative times the observe-only classifier
                                 // would have tripped WIFI_RECONN_ACTION_RECONNECT_NOW
                                 // (action is DISCARDED, never acted on)
    uint64_t last_gw_probe_ms;  // bb_clock_now_ms64() at the last probe, 0 = never run
} bb_wifi_gw_status_t;

// Populate out with the gateway-probe worker's last observed state.
// Returns BB_ERR_INVALID_ARG on NULL out. On ESP-IDF, returns
// BB_ERR_INVALID_STATE if the probe worker has never started (e.g.
// CONFIG_BB_WIFI_GW_PROBE_ENABLE=n). Host stub always returns BB_OK with a
// zeroed status (gw_reachable=false) unless overridden via
// bb_wifi_host_set_gateway_status (BB_WIFI_TESTING).
bb_err_t bb_wifi_get_gateway_status(bb_wifi_gw_status_t *out);

// Emit the canonical wifi section into a bb_json_t object.
// Writes: ssid, bssid (colon-hex), rssi (integer), ip, connected,
// disc_reason (integer), disc_age_s (integer), retry_count (integer).
// When disconnected all numeric fields are 0/false, strings empty/"0.0.0.0".
// Requires bb_json.h — callers that include bb_wifi.h must also link bb_json.
void bb_wifi_emit_section(bb_json_t obj, const bb_wifi_info_t *info);

// Emit status-only wifi fields into a bb_json_t object (TA-505).
// Writes: ssid, bssid (colon-hex), ip, connected — no numeric fields.
// Calls bb_wifi_get_info internally; no info parameter required.
void bb_wifi_emit_status(bb_json_t obj);

// ---------------------------------------------------------------------------
// Runtime WiFi reconfigure (brick-safe pending-creds try)
// ---------------------------------------------------------------------------

// Stage new WiFi credentials and reboot to try them. On boot, if the pending
// credentials succeed (got-IP), they are committed as the new live creds.
// On timeout they are cleared and the device reboots onto the prior live creds
// without incrementing boot_count. Returns BB_OK after staging and arming the
// deferred reboot; the response to the caller can flush before the restart
// fires. Returns BB_ERR_UNSUPPORTED when CONFIG_BB_WIFI_RECONFIGURE is off.
bb_err_t bb_wifi_reconfigure(const char *ssid, const char *pass);

// ---------------------------------------------------------------------------
// Transport (consumed by Arduino bb_http)
// ---------------------------------------------------------------------------
// Generic non-blocking accept-loop transport. The Arduino bb_http backend
// consumes it so it can stay radio-agnostic across WiFiS3 and CC3000
// shields. ESP-IDF bb_http uses esp_http_server directly and the ESP-IDF
// backend stubs these out as BB_ERR_INVALID_STATE.

typedef struct bb_conn bb_conn_t;

// Begin listening on port. Idempotent. The backend may defer the actual
// listen syscall until the first bb_wifi_accept call (CC3000 in particular
// requires server construction from loop() context, not init).
bb_err_t bb_wifi_listen(uint16_t port);

// Non-blocking accept. Returns BB_OK with *out set to a connection handle,
// or *out == NULL if no connection is pending. The handle is owned by the
// backend; release it with bb_conn_close.
bb_err_t bb_wifi_accept(bb_conn_t **out);

// Number of bytes available to read without blocking.
int  bb_conn_available(bb_conn_t *c);

// Read up to n bytes. Returns bytes read, 0 if none available, -1 on error.
int  bb_conn_read(bb_conn_t *c, uint8_t *buf, size_t n);

// Write up to n bytes. Returns bytes written, -1 on error.
int  bb_conn_write(bb_conn_t *c, const uint8_t *buf, size_t n);

// Close the connection and release the handle.
void bb_conn_close(bb_conn_t *c);

#ifdef __cplusplus
}
#endif

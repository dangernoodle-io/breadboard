#pragma once

// bb_settings — bb's default WiFi-credentials store, backed by bb_config.
//
// The wifi-creds field table forwards to bb_config_get_str/set_str/exists/
// erase over the SAME NVS namespace/keys bb_nv_config already uses
// ("bb_cfg"/"wifi_ssid"/"wifi_pass") — byte-compat with provisioned boards
// because bb_config's STR encoding routes through bb_storage's "nvs" backend
// get_typed/set_typed, which calls nvs_get_str/nvs_set_str under the hood
// (see bb_storage_nvs).
//
// bb_settings is bb's opinionated bb-config authority (KB 805/806): consumers
// that want bb's default wifi-creds store compose bb_settings and bb_wifi
// reads it directly via the accessors below. Nothing here self-registers.
//
// DEFERRED to later PRs (not in scope here): NVS lifecycle (factory-reset/
// boot-count/pending-creds) and bb_manifest dissolution.

#include "bb_core.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Read the stored WiFi SSID. Mirrors bb_config_get_str's size-probe/
// truncation contract: cap=0 probes the true length via out_len without
// touching buf. Returns BB_OK with an empty string (out_len=0, when
// out_len is non-NULL) when no SSID is stored.
//
// out_len MAY be NULL (#776 CRITICAL: bb_config_get_str itself rejects a
// NULL out_len with BB_ERR_INVALID_ARG, which would have left buf
// untouched and silently produced an empty SSID/pass at connect time). This
// accessor owns the NULL-safety guarantee: when out_len is NULL, it
// substitutes a local size_t so buf is always correctly filled and
// NUL-terminated regardless of whether the caller wants the length back.
bb_err_t bb_settings_wifi_ssid_get(char *buf, size_t cap, size_t *out_len);

// Read the stored WiFi password. Same size-probe/truncation contract and
// NULL-safe out_len guarantee as bb_settings_wifi_ssid_get. The value is
// SECRET — callers must never log it.
bb_err_t bb_settings_wifi_pass_get(char *buf, size_t cap, size_t *out_len);

// True iff a non-empty SSID is currently stored (non-empty-value semantics,
// not mere key presence — a present-but-empty SSID reports false).
bool bb_settings_wifi_has_creds(void);

// Read the stored hostname. Same size-probe/truncation contract and
// NULL-safe out_len guarantee as bb_settings_wifi_ssid_get. Returns BB_OK
// with an empty string (out_len=0) when unset — NO MAC-derived default,
// preserving bb_nv's prior empty-string-on-unset behavior exactly.
bb_err_t bb_settings_hostname_get(char *buf, size_t cap, size_t *out_len);

// Validate then persist a hostname (RFC 1123 / 952: letters, digits,
// hyphens; first/last cannot be hyphen; length 1..32). Validation runs
// BEFORE any persistence, fail-fast. Returns BB_ERR_INVALID_ARG for NULL,
// empty, >32 chars, bad charset, or leading/trailing hyphen; BB_OK on
// success.
bb_err_t bb_settings_hostname_set(const char *hostname);

// ---------------------------------------------------------------------------
// WiFi live-creds writer (B1: bb_nv creds-cluster PR4).
//
// Writes the LIVE ssid/pass fields directly (the same fields
// bb_settings_wifi_ssid_get/bb_settings_wifi_pass_get read, and bb_wifi
// connects with) -- unlike bb_settings_wifi_pending_set below, there is no
// staged/try-flag/promote step; this lands immediately. No WiFi validation
// lives here -- callers pre-validate ssid/pass (same posture as the
// pending-creds writers).
// ---------------------------------------------------------------------------

// Write the live WiFi ssid/pass: pass (NULL treated as empty -- open
// network) and ssid are written atomically in one bb_config_staged session
// (2 keys, all-or-nothing) against the same live fields
// bb_settings_wifi_ssid_get/bb_settings_wifi_pass_get read. NOTE the ssid/
// pass NULL asymmetry: a NULL pass is a valid "open network" request and is
// substituted with "" before staging, but a NULL ssid is NOT substituted --
// it is passed straight to the staged setter, which returns
// BB_ERR_INVALID_ARG (via the commit) without touching storage. An oversize
// ssid/pass (beyond the field's max_len) fails the same staged precheck
// (BB_ERR_INVALID_ARG) WITHOUT touching storage. After a successful commit,
// the RTC warm-reboot mirror is best-effort updated (see
// bb_settings_wifi_pending_promote's doc for the mirror's crash/
// availability contract) -- a failure there never affects this call's
// BB_OK return, since the NVS commit already succeeded and is
// authoritative.
bb_err_t bb_settings_wifi_set(const char *ssid, const char *pass);

// ---------------------------------------------------------------------------
// WiFi pending-creds writers (B1: bb_nv creds-cluster PR2).
//
// Mirrors bb_nv's pending-creds lifecycle (platform/espidf/bb_nv/bb_nv.c's
// set_wifi_pending/commit_wifi_pending/clear_wifi_pending) over the SAME
// "bb_cfg" NVS namespace and byte-compat key names (wifi_ssid_p/wifi_pass_p/
// wifi_try), built on bb_config_staged's atomic multi-field commit. PURELY
// ADDITIVE here: nothing on the live boot path calls these yet (that switch
// is a later, HW-gated PR) — bb_nv's existing pending path is untouched.
//
// No WiFi validation lives here — callers pre-validate ssid/pass (mirrors
// bb_wifi_pending_validate's job, owned upstream, not duplicated here).
// ---------------------------------------------------------------------------

// Stage a new pending WiFi reconfigure attempt: ssid, pass (NULL treated as
// empty -- open network), and the try flag are written atomically in one
// bb_config_staged session (3 keys, all-or-nothing). No validation --
// callers pre-validate.
bb_err_t bb_settings_wifi_pending_set(const char *ssid, const char *pass);

// Read the staged pending SSID. Same size-probe/truncation contract and
// NULL-safe out_len guarantee as bb_settings_wifi_ssid_get. Returns BB_OK
// with an empty string (out_len=0) when unset.
bb_err_t bb_settings_wifi_pending_ssid_get(char *buf, size_t cap, size_t *out_len);

// Read the staged pending password. Same contract as
// bb_settings_wifi_pending_ssid_get. SECRET -- callers must never log it.
bb_err_t bb_settings_wifi_pending_pass_get(char *buf, size_t cap, size_t *out_len);

// True iff a pending reconfigure attempt should be tried: the try flag is
// set AND the pending SSID is non-empty. A pure storage read of the same
// gate bb_nv's bb_wifi_pending_decide implements -- NOT wifi policy;
// bb_wifi still owns the decide orchestration.
bool bb_settings_wifi_pending_active(void);

// Promote the staged pending creds to live: live ssid/pass are overwritten
// with the pending values and the try flag is cleared, atomically (3 keys,
// one bb_config_staged commit -- the live-creds swap and try-clear cannot
// tear). Returns BB_ERR_INVALID_STATE if no pending SSID is staged (guard
// checked before anything is touched). After a successful atomic commit,
// the plaintext pending bytes are erased on a BEST-EFFORT basis (their
// return is ignored) -- try=0 already committed is the crash-safe decision
// bit, so a failed/incomplete erase just leaves harmless stale bytes. The
// RTC warm-reboot mirror ("rtc" bb_storage backend, ssid/pass/provisioned
// keys) is ALSO best-effort updated after the atomic commit succeeds --
// same rationale as bb_settings_wifi_set: the NVS commit is authoritative,
// the mirror write's own success/failure never changes this call's return.
// A backend error or no "rtc" backend registered at all is silently
// swallowed (fail-open) -- the mirror is a recovery cache, not required for
// correctness.
bb_err_t bb_settings_wifi_pending_promote(void);

// Discard any pending reconfigure attempt: clears the try flag, then
// best-effort erases the plaintext pending ssid/pass bytes (their return is
// ignored, same rationale as bb_settings_wifi_pending_promote). Idempotent
// -- returns BB_OK whether or not a pending attempt was active.
bb_err_t bb_settings_wifi_pending_clear(void);

#ifdef __cplusplus
}
#endif

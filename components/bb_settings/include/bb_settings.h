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
// boot-count/pending-creds).

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

// True iff `ns` is the NVS namespace bb_settings stores wifi credentials
// under. False for NULL. Lets a generic consumer (e.g. bb_diag_http's DELETE
// /api/diag/storage route, B1-757, dissolved in from bb_storage_http per
// B1-1154) ask "does this namespace hold wifi creds?" without
// copying bb_settings' namespace literal itself — a copy would silently go
// stale if that namespace ever changed, defeating the safety guard it backs.
bool bb_settings_ns_is_wifi_creds(const char *ns);

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

// Read the stored POSIX timezone string (e.g. "EST5EDT,M3.2.0,M11.1.0").
// Same size-probe/truncation contract and NULL-safe out_len guarantee as
// bb_settings_wifi_ssid_get. Returns BB_OK with an empty string (out_len=0)
// when unset -- UTC applies -- preserving bb_nv's prior empty-string-on-unset
// behavior exactly (B1-750).
bb_err_t bb_settings_timezone_get(char *buf, size_t cap, size_t *out_len);

// Persist a POSIX timezone string. NULL or empty clears to "" (UTC applies).
// No charset validation (unlike hostname) -- only a length check (>64 chars
// rejected), mirroring bb_nv_config_set_timezone's prior contract exactly
// (B1-750). Returns BB_ERR_INVALID_ARG when tz is longer than 64 chars;
// BB_OK on success.
bb_err_t bb_settings_timezone_set(const char *tz);

// Read the display-enabled flag. Fail-open to true (display enabled) on
// both an unset key AND a real backend I/O error -- mirrors bb_nv's prior
// "nvs_get_u8(...) != ESP_OK -> default 1" behavior exactly (B1-750): a
// storage error was never distinguishable from "unset" there either.
bool bb_settings_display_enabled_get(void);

// Persist the display-enabled flag.
bb_err_t bb_settings_display_enabled_set(bool en);

// Read the mDNS-enabled flag. Same fail-open-to-true contract as
// bb_settings_display_enabled_get (B1-750).
bool bb_settings_mdns_enabled_get(void);

// Persist the mDNS-enabled flag.
bb_err_t bb_settings_mdns_enabled_set(bool en);

// Read the update-check-enabled flag. Same fail-open-to-true contract as
// bb_settings_display_enabled_get (B1-750).
bool bb_settings_update_check_enabled_get(void);

// Persist the update-check-enabled flag.
bb_err_t bb_settings_update_check_enabled_set(bool en);

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

// ---------------------------------------------------------------------------
// RTC warm-reboot mirror accessors (B1: bb_nv creds-cluster relocation --
// bb_nv's heal/seed/factory-reset paths read and invalidate the SAME shared
// "rtc" bb_storage backend bb_settings_wifi_set/_pending_promote already
// mirror onto, rather than owning a private RTC region of their own).
// ---------------------------------------------------------------------------

// True iff the RTC mirror currently holds a non-empty SSID (per rtc_exists'
// own semantics: region valid AND ssid non-empty) -- the exact gate a heal
// or seed decision needs ("is there anything here worth reading/worth NOT
// overwriting").
bool bb_settings_wifi_rtc_mirror_has_creds(void);

// Read the RTC-mirrored SSID. Same size-probe/truncation contract and
// NULL-safe out_len guarantee as bb_settings_wifi_ssid_get. Neither mirror
// field carries a has_default (matching the LIVE wifi.ssid/wifi.pass fields'
// own contract, bb_settings_wifi_ssid_get/_pass_get) -- returns
// BB_ERR_NOT_FOUND, not BB_OK+empty, when the mirror is empty/invalid/
// unregistered. Callers must check bb_settings_wifi_rtc_mirror_has_creds()
// first rather than relying on this getter's error to distinguish "empty"
// from "unregistered backend"/other I/O failure.
bb_err_t bb_settings_wifi_rtc_mirror_ssid_get(char *buf, size_t cap, size_t *out_len);

// Read the RTC-mirrored password. Same contract (including the
// BB_ERR_NOT_FOUND-not-BB_OK-when-empty note) as
// bb_settings_wifi_rtc_mirror_ssid_get. SECRET -- callers must never log it.
bb_err_t bb_settings_wifi_rtc_mirror_pass_get(char *buf, size_t cap, size_t *out_len);

// True iff the RTC mirror's "provisioned" key reads back non-zero. Fail-
// CLOSED (false) on any backend error or an invalid/unregistered mirror --
// unlike the display/mdns/update-check flags above, a storage error here
// must never be treated as "yes, provisioned", since this gates whether a
// heal re-marks a recovered board as provisioned.
bool bb_settings_wifi_rtc_mirror_provisioned_get(void);

// Invalidate the RTC mirror (whole-region erase -- rtc_erase invalidates
// regardless of which key is passed, see bb_storage_rtc). Best-effort
// semantics are the CALLER's choice here (unlike the write paths above,
// which are always best-effort/fail-open); this wrapper returns whatever
// bb_storage_erase itself returns, including BB_ERR_NOT_FOUND when no "rtc"
// backend is registered at all.
bb_err_t bb_settings_wifi_rtc_mirror_clear(void);

// Best-effort mirror of ssid/pass into the "rtc" bb_storage backend (single
// atomic bb_config_staged commit -- ssid/pass/provisioned=1 land together or
// not at all). Exposed publicly so callers OUTSIDE bb_settings.c (formerly
// bb_nv's init-time mirror-seed and provisioned-flag repack, now
// bb_settings_creds_boot_init below) can arm/refresh the mirror without
// duplicating this component's field descriptors or cross-backend staging
// precheck. pass may be NULL (treated as empty). Fail-open: a missing "rtc"
// backend, or any backend error, is silently swallowed -- the mirror is a
// recovery cache, never required for correctness. See bb_settings_wifi_set's
// doc for the full crash/availability contract this mirror write follows.
void bb_settings_wifi_rtc_mirror_write(const char *ssid, const char *pass);

// ---------------------------------------------------------------------------
// Creds-boot heal/seed shell (B1-963/B1-708: relocated VERBATIM from
// platform/espidf/bb_nv/bb_nv.c's bb_nv_config_init() -- the shell's own
// delegates (bb_storage_nvs_flash_init(), the pure heal-vs-seed decision now
// at components/bb_settings/src/bb_settings_creds_boot_decide.c, and the
// bb_settings_wifi_* accessors above) are unchanged; only the caller moved).
// ---------------------------------------------------------------------------

// Bring up the NVS partition, then (CONFIG_BB_SETTINGS_CREDS_RTC_BACKUP-gated)
// decide and execute the RTC-mirror heal-vs-seed action for wifi creds. ESP-
// IDF only (a no-op returning BB_OK on host, matching bb_nv_config_init's
// prior host stub exactly). requires=storage_rtc: this reads/writes the
// shared "rtc" bb_storage backend via the mirror accessors above, which
// needs bb_storage_rtc_register() (provides=storage_rtc) to have already run
// in the same EARLY tier -- see bb_settings_creds_boot_init's implementation
// comment (platform/host/bb_settings/bb_settings.c) for the full ordering
// rationale, preserved verbatim from bb_nv_config_init.
// bbtool:init tier=early fn=bb_settings_creds_boot_init requires=storage_rtc
bb_err_t bb_settings_creds_boot_init(void);

// ---------------------------------------------------------------------------
// NVS-key schema overlay (B1-708 PR7): a read-only projection over the
// bb_config_field_t literals bb_settings already declares -- replaces the
// hand-duplicated manifest registration this component used to own
// (bb_settings_creds_boot_manifest_init/s_creds_boot_manifest_keys, deleted
// here; this was the FIRST manifest-registration caller removed, a
// precondition for that component's own eventual deletion, now complete).
// Pure schema metadata, no storage
// access -- mirrors the old manifest registration's exact scope (the 3 live
// "bb_cfg" keys: wifi_ssid/wifi_pass/provisioned), not the pending-creds or
// RTC-mirror fields (least-surprise; a future consumer can widen this if it
// wants the fuller picture).
// ---------------------------------------------------------------------------

// One NVS-key's schema metadata, derived from a bb_config_field_t literal.
// Distinct from bb_storage_entry_t (PR5, LIVE-value shaped) -- this carries
// the render-metadata bb_config_field_t has that bb_storage_entry_t doesn't
// (label/secret/provisioning_only/reboot_required).
typedef struct {
    const char *ns_or_dir;
    const char *key;
    const char *type_str;        // "bool"|"u8"|"u16"|"u32"|"i32"|"str"|"blob"
    const char *label;
    bool        secret;
    bool        provisioning_only;
    bool        reboot_required;
} bb_settings_nv_overlay_entry_t;

// Compile-time capacity: the known field count this component's overlay
// covers today (wifi_ssid/wifi_pass/provisioned).
#define BB_SETTINGS_NV_OVERLAY_CAP 3

// Fills out[0..min(cap, total))] with the NVS-key schema metadata for every
// bb_config_field_t this overlay covers, in declaration order. Returns the
// TOTAL entry count found, even when it exceeds cap (same truncation-report
// contract as bb_storage_list_entries) -- callers detect truncation via
// return value > cap. Pure: no storage access, schema only.
size_t bb_settings_nv_overlay_entries(bb_settings_nv_overlay_entry_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

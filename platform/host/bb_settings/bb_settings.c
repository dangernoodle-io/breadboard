// bb_settings — default WiFi-credentials store backed by bb_config.
//
// Portable (no ESP-IDF deps): compiled on host and ESP-IDF, mirroring
// bb_config and bb_dispatch_cmd. The field table below targets the SAME
// NVS namespace/keys bb_nv_config already uses ("bb_cfg"/"wifi_ssid"/
// "wifi_pass") -- see components/bb_settings/include/bb_settings.h for the
// byte-compat rationale.

#include "bb_settings.h"
#include "bb_config.h"
#include "bb_config_staged.h"
#include <string.h>

// Namespace/keys/max-lengths byte-for-byte matched to
// platform/espidf/bb_nv/bb_nv.c's BB_NV_KEY_WIFI_SSID/BB_NV_KEY_WIFI_PASS
// under BB_NV_CONFIG_NVS_NS ("bb_cfg") -- do not change without a migration
// plan, this strands provisioned-board credentials otherwise.
#define BB_SETTINGS_WIFI_NS       "bb_cfg"
#define BB_SETTINGS_WIFI_SSID_KEY "wifi_ssid"
#define BB_SETTINGS_WIFI_PASS_KEY "wifi_pass"

// Byte-compat with bb_nv: matches platform/espidf/bb_nv/bb_nv.c's
// BB_NV_KEY_HOSTNAME under the SAME BB_SETTINGS_WIFI_NS ("bb_cfg") -- do not
// change without a migration plan, this strands provisioned-board hostnames
// otherwise (B1-754).
#define BB_SETTINGS_HOSTNAME_KEY "hostname"

// Buffer sizes mirror bb_nv's s_config.wifi_ssid[32]/wifi_pass[64] exactly.
static const bb_config_field_t s_wifi_ssid_field = {
    .id      = "wifi.ssid",
    .type    = BB_CONFIG_STR,
    .addr    = { .backend = "nvs", .ns_or_dir = BB_SETTINGS_WIFI_NS, .key = BB_SETTINGS_WIFI_SSID_KEY },
    .max_len = 32,
    .label   = "WiFi SSID",
    .group   = "network",
};

static const bb_config_field_t s_wifi_pass_field = {
    .id      = "wifi.pass",
    .type    = BB_CONFIG_STR,
    .addr    = { .backend = "nvs", .ns_or_dir = BB_SETTINGS_WIFI_NS, .key = BB_SETTINGS_WIFI_PASS_KEY },
    .max_len = 64,
    .label   = "WiFi Password",
    .group   = "network",
    .secret  = true,
};

// Pending-creds field keys/max-lengths byte-for-byte matched to
// platform/espidf/bb_nv/bb_nv.c's BB_NV_KEY_WIFI_SSID_P/BB_NV_KEY_WIFI_PASS_P/
// BB_NV_KEY_WIFI_TRY under the SAME BB_SETTINGS_WIFI_NS ("bb_cfg") -- do not
// change without a migration plan, this strands provisioned-board pending
// creds otherwise. ssid_p/pass_p max_len mirror s_wifi_ssid_field/
// s_wifi_pass_field exactly (32/64) so a pending value is byte-compatible
// with what bb_nv writes. Unlike the live ssid/pass fields above, ssid_p/
// pass_p carry has_default="" so an unset pending value resolves to BB_OK +
// empty string (same empty-on-unset contract as s_hostname_field below)
// rather than propagating BB_ERR_NOT_FOUND -- pending-creds callers (and
// bb_settings_wifi_pending_active's gate) need a clean "nothing pending"
// read, not an error to special-case.
#define BB_SETTINGS_WIFI_SSID_P_KEY "wifi_ssid_p"
#define BB_SETTINGS_WIFI_PASS_P_KEY "wifi_pass_p"
#define BB_SETTINGS_WIFI_TRY_KEY    "wifi_try"

static const bb_config_field_t s_wifi_ssid_p_field = {
    .id          = "wifi.ssid_pending",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_SETTINGS_WIFI_NS, .key = BB_SETTINGS_WIFI_SSID_P_KEY },
    .max_len     = 32,
    .def         = { .str = "" },
    .has_default = true,
    .label       = "WiFi SSID (pending)",
    .group       = "network",
};

static const bb_config_field_t s_wifi_pass_p_field = {
    .id          = "wifi.pass_pending",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_SETTINGS_WIFI_NS, .key = BB_SETTINGS_WIFI_PASS_P_KEY },
    .max_len     = 64,
    .def         = { .str = "" },
    .has_default = true,
    .label       = "WiFi Password (pending)",
    .group       = "network",
    .secret      = true,
};

static const bb_config_field_t s_wifi_try_field = {
    .id    = "wifi.try_pending",
    .type  = BB_CONFIG_U8,
    .addr  = { .backend = "nvs", .ns_or_dir = BB_SETTINGS_WIFI_NS, .key = BB_SETTINGS_WIFI_TRY_KEY },
    .label = "WiFi pending-creds try flag",
    .group = "network",
};

// Buffer size mirrors bb_nv's s_config.hostname[33] (32 chars + NUL) exactly.
// max_len=33 (not 32): bb_config_set_str rejects len >= max_len, so max_len
// is BUFFER CAPACITY (usable chars = max_len-1), same convention as the
// wifi-creds fields above -- a max_len of 32 would wrongly reject a valid
// 32-char hostname.
// has_default="" (NOT a MAC-derived default) so an unset hostname resolves
// to BB_OK + empty string rather than propagating BB_ERR_NOT_FOUND --
// preserves bb_nv's prior empty-string-on-unset behavior exactly (B1-754).
static const bb_config_field_t s_hostname_field = {
    .id          = "hostname",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_SETTINGS_WIFI_NS, .key = BB_SETTINGS_HOSTNAME_KEY },
    .max_len     = 33,
    .def         = { .str = "" },
    .has_default = true,
    .label       = "Hostname",
    .group       = "network",
};

// NULL-safe out_len (#776 CRITICAL): bb_config_get_str rejects a NULL
// out_len outright (BB_ERR_INVALID_ARG), which would leave buf untouched --
// a caller passing NULL because it doesn't need the length back would
// silently get an empty SSID/pass. This accessor owns the guarantee: when
// out_len is NULL, substitute a local size_t so buf is always correctly
// filled regardless of whether the caller wants the length back.
bb_err_t bb_settings_wifi_ssid_get(char *buf, size_t cap, size_t *out_len)
{
    size_t len = 0;
    return bb_config_get_str(&s_wifi_ssid_field, buf, cap, out_len ? out_len : &len);
}

// Never log the returned password value -- secret=true on the field
// descriptor above documents this; callers must honor it too. Same
// NULL-safe out_len guarantee as bb_settings_wifi_ssid_get.
bb_err_t bb_settings_wifi_pass_get(char *buf, size_t cap, size_t *out_len)
{
    size_t len = 0;
    return bb_config_get_str(&s_wifi_pass_field, buf, cap, out_len ? out_len : &len);
}

// Non-empty-value semantics -- matches bb_wifi's fallback wifi_has_creds()
// (ssid[0] != '\0'), NOT mere key presence. A cap=0 call is a valid
// bb_config_get_str size-probe (returns the true length via out_len without
// touching buf); an empty-but-present ssid key probes to out_len==0 and
// correctly reports "no creds", keeping bb_settings and the fallback path
// zero-drift.
bool bb_settings_wifi_has_creds(void)
{
    size_t len = 0;
    bb_err_t err = bb_config_get_str(&s_wifi_ssid_field, NULL, 0, &len);
    return err == BB_OK && len > 0;
}

// Same NULL-safe out_len guarantee as bb_settings_wifi_ssid_get. Returns
// BB_OK with an empty string (out_len=0) when unset -- no MAC-derived
// default, matching bb_nv's prior behavior exactly.
bb_err_t bb_settings_hostname_get(char *buf, size_t cap, size_t *out_len)
{
    size_t len = 0;
    return bb_config_get_str(&s_hostname_field, buf, cap, out_len ? out_len : &len);
}

// RFC 1123 / 952: letters, digits, hyphens; first/last cannot be hyphen;
// length 1..32. Tolerant of mixed case (DHCP / mDNS treat case-insensitively).
// Moved from bb_nv's nv_valid_hostname() (B1-754).
static bool settings_valid_hostname(const char *s)
{
    if (!s) return false;
    size_t len = strlen(s);
    if (len == 0 || len > 32) return false;
    if (s[0] == '-' || s[len - 1] == '-') return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

// Validates before any persistence (fail-fast, mirrors bb_nv's
// nv_valid_hostname order).
bb_err_t bb_settings_hostname_set(const char *hostname)
{
    if (!settings_valid_hostname(hostname)) return BB_ERR_INVALID_ARG;
    return bb_config_set_str(&s_hostname_field, hostname);
}

// ---------------------------------------------------------------------------
// WiFi pending-creds writers (B1: bb_nv creds-cluster PR2). No validation --
// callers pre-validate ssid/pass (mirrors bb_wifi_pending_validate's job,
// owned upstream by bb_nv today, not duplicated here).
// ---------------------------------------------------------------------------

// 3 keys, one atomic bb_config_staged commit -- all-or-nothing.
bb_err_t bb_settings_wifi_pending_set(const char *ssid, const char *pass)
{
    const char *p = pass ? pass : "";

    bb_config_staged_t h = {0};
    bb_err_t err = bb_config_staged_begin(&h, "nvs", BB_SETTINGS_WIFI_NS);
    if (err != BB_OK) return err;

    bb_config_staged_set_str(&h, &s_wifi_ssid_p_field, ssid);
    bb_config_staged_set_str(&h, &s_wifi_pass_p_field, p);
    bb_config_staged_set_u8(&h, &s_wifi_try_field, 1);
    return bb_config_staged_commit(&h);
}

// Same NULL-safe out_len guarantee as bb_settings_wifi_ssid_get.
bb_err_t bb_settings_wifi_pending_ssid_get(char *buf, size_t cap, size_t *out_len)
{
    size_t len = 0;
    return bb_config_get_str(&s_wifi_ssid_p_field, buf, cap, out_len ? out_len : &len);
}

// Never log the returned password value -- secret=true on the field
// descriptor above documents this. Same NULL-safe out_len guarantee.
bb_err_t bb_settings_wifi_pending_pass_get(char *buf, size_t cap, size_t *out_len)
{
    size_t len = 0;
    return bb_config_get_str(&s_wifi_pass_p_field, buf, cap, out_len ? out_len : &len);
}

// Pure storage read of the same try!=0 AND ssid-non-empty gate bb_nv's
// bb_wifi_pending_decide implements (components/bb_nv/bb_nv_wifi_pending.c)
// -- reimplemented locally rather than depending on bb_nv, since bb_settings
// is bb_nv's replacement surface here (a bb_settings -> bb_nv dependency
// would point the wrong direction). NOT wifi policy; bb_wifi still owns the
// decide orchestration.
// Deliberate fail-closed: a real backend error on either read is treated
// the same as "unset" (try_flag=0 / serr!=BB_OK) -- a storage error means
// "treat as no-pending", never "assume pending". A future consumer
// (bb_wifi orchestration, PR5) wanting to distinguish "no pending" from
// "storage degraded" would need a separate signal; this call site can't
// tell the two apart today.
bool bb_settings_wifi_pending_active(void)
{
    uint8_t try_flag = 0;
    bb_err_t terr = bb_config_get_u8(&s_wifi_try_field, &try_flag);
    if (terr != BB_OK) try_flag = 0;  // unset/not-found -- no pending try

    char ssid[32] = {0};
    size_t ssid_len = 0;
    bb_err_t serr = bb_config_get_str(&s_wifi_ssid_p_field, ssid, sizeof(ssid), &ssid_len);

    return try_flag != 0 && serr == BB_OK && ssid_len > 0 && ssid[0] != '\0';
}

// PROMOTE: read pending ssid/pass into local buffers BEFORE opening the
// staged session, then stage the live writes from those buffers -- live
// ssid/pass + try-clear land atomically in one bb_config_staged commit (3
// keys, cannot tear). Cleanup of the plaintext pending bytes is
// best-effort, AFTER the atomic commit -- try=0 already committed is the
// crash-safe decision bit (see bb_settings_wifi_pending_active), so a
// failed/incomplete erase just leaves harmless stale bytes.
//
// NOTE: this does NOT repack bb_nv's RTC creds-mirror (CONFIG_BB_NV_CREDS_
// RTC_BACKUP) -- that mirror is bb_nv-internal today and relocating it is
// PR3/PR4's job. Nothing calls promote on the live path yet (this PR is
// purely additive), so the mirror isn't stranded by this omission.
bb_err_t bb_settings_wifi_pending_promote(void)
{
    // Deliberate fail-closed: a real backend error on the ssid read aborts
    // promote (treated as "no pending" -- see bb_settings_wifi_pending_active).
    char ssid[32] = {0};
    size_t ssid_len = 0;
    bb_err_t serr = bb_config_get_str(&s_wifi_ssid_p_field, ssid, sizeof(ssid), &ssid_len);
    if (serr != BB_OK || ssid_len == 0) return BB_ERR_INVALID_STATE;

    // Fail closed on a real backend error here too: NOT_FOUND is already
    // absorbed to BB_OK + "" by has_default, so a non-OK here means a
    // genuine I/O failure -- abort rather than commit an empty pass over a
    // live one. A legitimately-empty pass (open network) still promotes
    // fine, since that comes from a BB_OK read of an empty value, not a
    // swallowed error.
    char pass[64] = {0};
    size_t pass_len = 0;
    bb_err_t perr = bb_config_get_str(&s_wifi_pass_p_field, pass, sizeof(pass), &pass_len);
    if (perr != BB_OK) return perr;
    (void)pass_len;

    bb_config_staged_t h = {0};
    bb_err_t err = bb_config_staged_begin(&h, "nvs", BB_SETTINGS_WIFI_NS);
    if (err != BB_OK) return err;

    bb_config_staged_set_str(&h, &s_wifi_ssid_field, ssid);
    bb_config_staged_set_str(&h, &s_wifi_pass_field, pass);
    bb_config_staged_set_u8(&h, &s_wifi_try_field, 0);
    err = bb_config_staged_commit(&h);
    if (err != BB_OK) return err;

    // Best-effort -- return values ignored, see function comment above.
    bb_storage_erase(&s_wifi_ssid_p_field.addr);
    bb_storage_erase(&s_wifi_pass_p_field.addr);
    return BB_OK;
}

// DISCARD: clears the try flag (the crash-safe decision bit), then
// best-effort erases the plaintext pending bytes -- same rationale as
// bb_settings_wifi_pending_promote. Idempotent: BB_OK whether or not a
// pending try was active.
bb_err_t bb_settings_wifi_pending_clear(void)
{
    bb_err_t err = bb_config_set_u8(&s_wifi_try_field, 0);

    // Best-effort -- return values ignored, see function comment above.
    bb_storage_erase(&s_wifi_ssid_p_field.addr);
    bb_storage_erase(&s_wifi_pass_p_field.addr);

    return err;
}

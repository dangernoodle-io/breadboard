// bb_settings — default WiFi-credentials store backed by bb_config.
//
// Portable (no ESP-IDF deps): compiled on host and ESP-IDF, mirroring
// bb_config. The field table below targets the SAME
// NVS namespace/keys bb_nv_config already uses ("bb_cfg"/"wifi_ssid"/
// "wifi_pass") -- see components/bb_settings/include/bb_settings.h for the
// byte-compat rationale.

#include "bb_settings.h"
#include "bb_config.h"
#include "bb_config_staged.h"
#include "bb_storage.h"
#include "bb_settings_creds_boot_decide.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "bb_log.h"
#include "bb_storage_nvs.h"
#endif

// Namespace/keys/max-lengths byte-for-byte matched to
// platform/espidf/bb_nv/bb_nv.c's BB_NV_KEY_WIFI_SSID/BB_NV_KEY_WIFI_PASS
// under BB_NV_CONFIG_NVS_NS ("bb_cfg") -- do not change without a migration
// plan, this strands provisioned-board credentials otherwise.
#define BB_SETTINGS_WIFI_NS       "bb_cfg"
#define BB_SETTINGS_WIFI_SSID_KEY "wifi_ssid"
#define BB_SETTINGS_WIFI_PASS_KEY "wifi_pass"

// Byte-compat with bb_nv: matches platform/espidf/bb_nv/bb_nv.c's
// BB_NV_KEY_PROVISIONED under the SAME BB_SETTINGS_WIFI_NS ("bb_cfg") --
// do not change without a migration plan, this strands provisioned-board
// provisioning state otherwise (B1-963).
#define BB_SETTINGS_PROVISIONED_KEY "provisioned"

#ifdef ESP_PLATFORM
static const char *TAG = "bb_settings";
#endif

// Byte-compat with bb_nv: matches platform/espidf/bb_nv/bb_nv.c's
// BB_NV_KEY_HOSTNAME under the SAME BB_SETTINGS_WIFI_NS ("bb_cfg") -- do not
// change without a migration plan, this strands provisioned-board hostnames
// otherwise (B1-754).
#define BB_SETTINGS_HOSTNAME_KEY "hostname"

// Byte-compat with bb_nv: matches platform/espidf/bb_nv/bb_nv.c's
// BB_NV_KEY_TIMEZONE under the SAME BB_SETTINGS_WIFI_NS ("bb_cfg") -- do not
// change without a migration plan, this strands provisioned-board timezones
// otherwise (B1-750).
#define BB_SETTINGS_TIMEZONE_KEY "timezone"

// Buffer sizes mirror bb_nv's s_config.wifi_ssid[32]/wifi_pass[64] exactly.
// Capacity coupling (not compiler-enforced, deliberately -- see
// settings_wifi_rtc_mirror_write's comment on why bb_settings must NOT take
// a bb_storage_rtc include/dependency): max_len below must stay
// byte-compatible with bb_storage_rtc_region_t.ssid[32]/.pass[64]
// (components/bb_storage_rtc/include/bb_storage_rtc_region.h) or the RTC
// mirror write silently BB_ERR_NO_SPACE-fails (still fail-open/BB_OK to the
// caller, just a silently-degraded mirror). Editing either side, check the
// other.
static const bb_config_field_t s_wifi_ssid_field = {
    .id                = "wifi.ssid",
    .type              = BB_CONFIG_STR,
    .addr              = { .backend = "nvs", .ns_or_dir = BB_SETTINGS_WIFI_NS, .key = BB_SETTINGS_WIFI_SSID_KEY },
    .max_len           = 32,
    .label             = "WiFi SSID",
    .group             = "network",
    // provisioning_only/reboot_required: metadata-only (not part of the NVS
    // byte-compat surface -- see max_len/addr/key above), filled in here to
    // match the values the now-deleted s_creds_boot_manifest_keys[] carried
    // (B1-708 PR7 -- these were "carried but unconsumed" per bb_config.h
    // until this overlay became the schema consumer).
    .provisioning_only = true,
    .reboot_required   = true,
};

static const bb_config_field_t s_wifi_pass_field = {
    .id                = "wifi.pass",
    .type              = BB_CONFIG_STR,
    .addr              = { .backend = "nvs", .ns_or_dir = BB_SETTINGS_WIFI_NS, .key = BB_SETTINGS_WIFI_PASS_KEY },
    .max_len           = 64,
    .label             = "WiFi Password",
    .group             = "network",
    .secret            = true,
    // provisioning_only/reboot_required: see s_wifi_ssid_field's comment above.
    .provisioning_only = true,
    .reboot_required   = true,
};

// Live NVS "provisioned" flag (distinct from s_wifi_rtc_provisioned_field
// below, which is the RTC MIRROR's copy) -- BB_CONFIG_BOOL encodes as
// BB_STORAGE_ENC_U8 (see bb_config_type_to_enc), byte-identical to
// bb_nv_config_set_provisioned's prior direct nvs_set_u8(..., "provisioned",
// 1) call. Used only by bb_settings_creds_boot_init's heal branch below to
// re-stamp the flag after an RTC-mirror-driven creds restore.
static const bb_config_field_t s_wifi_provisioned_field = {
    .id              = "wifi.provisioned",
    .type            = BB_CONFIG_BOOL,
    .addr            = { .backend = "nvs", .ns_or_dir = BB_SETTINGS_WIFI_NS, .key = BB_SETTINGS_PROVISIONED_KEY },
    .label           = "Provisioning completed flag",
    .group           = "network",
    // reboot_required: metadata-only, see s_wifi_ssid_field's comment above
    // (matches s_creds_boot_manifest_keys[]'s prior "provisioned" entry).
    .reboot_required = true,
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

// Buffer size mirrors bb_nv's s_config.timezone[65] (64 chars + NUL) exactly
// (BB_NV_TIMEZONE_MAX_LEN in platform/espidf/bb_nv/bb_nv.c). max_len=65 is
// BUFFER CAPACITY (usable chars = max_len-1), same convention as the
// hostname/wifi-creds fields above.
// has_default="" (empty-on-unset, UTC applies) -- preserves bb_nv's prior
// behavior exactly (B1-750).
static const bb_config_field_t s_timezone_field = {
    .id          = "timezone",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_SETTINGS_WIFI_NS, .key = BB_SETTINGS_TIMEZONE_KEY },
    .max_len     = 65,
    .def         = { .str = "" },
    .has_default = true,
    .label       = "Timezone",
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

// B1-757: the one place BB_SETTINGS_WIFI_NS is compared against an
// external string -- generic consumers ask this predicate instead of
// copying the "bb_cfg" literal themselves.
bool bb_settings_ns_is_wifi_creds(const char *ns)
{
    return ns != NULL && strcmp(ns, BB_SETTINGS_WIFI_NS) == 0;
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

// Same NULL-safe out_len guarantee as bb_settings_wifi_ssid_get. Returns
// BB_OK with an empty string (out_len=0) when unset -- UTC applies -- moved
// from bb_nv's bb_nv_config_timezone() (B1-750).
bb_err_t bb_settings_timezone_get(char *buf, size_t cap, size_t *out_len)
{
    size_t len = 0;
    return bb_config_get_str(&s_timezone_field, buf, cap, out_len ? out_len : &len);
}

// No charset validation (unlike hostname) -- only a length check, mirroring
// bb_nv_config_set_timezone's prior contract exactly (B1-750): NULL/empty
// clears to "", >64 chars rejected with BB_ERR_INVALID_ARG.
bb_err_t bb_settings_timezone_set(const char *tz)
{
    const char *t = (tz && tz[0] != '\0') ? tz : "";
    if (strlen(t) > 64) return BB_ERR_INVALID_ARG;
    return bb_config_set_str(&s_timezone_field, t);
}

// Byte-compat with bb_nv: matches platform/espidf/bb_nv/bb_nv.c's (removed)
// BB_NV_KEY_DISPLAY_EN/BB_NV_KEY_MDNS_EN/BB_NV_KEY_UPDATE_CHECK_EN under the
// SAME BB_SETTINGS_WIFI_NS ("bb_cfg") -- do not change without a migration
// plan, this strands provisioned-board flag values otherwise (B1-750).
#define BB_SETTINGS_DISPLAY_EN_KEY       "display_en"
#define BB_SETTINGS_MDNS_EN_KEY          "mdns_en"
#define BB_SETTINGS_UPDATE_CHECK_EN_KEY  "update_check_en"

// Default-on (true) when unset -- preserves bb_nv_config_init's prior
// "no config in NVS" / nvs_get_u8-failure default-on behavior exactly
// (was platform/espidf/bb_nv/bb_nv.c, B1-750).
static const bb_config_field_t s_display_enabled_field = {
    .id          = "display.enabled",
    .type        = BB_CONFIG_BOOL,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_SETTINGS_WIFI_NS, .key = BB_SETTINGS_DISPLAY_EN_KEY },
    .def         = { .b = true },
    .has_default = true,
    .label       = "Display enabled",
    .group       = "system",
};

static const bb_config_field_t s_mdns_enabled_field = {
    .id          = "mdns.enabled",
    .type        = BB_CONFIG_BOOL,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_SETTINGS_WIFI_NS, .key = BB_SETTINGS_MDNS_EN_KEY },
    .def         = { .b = true },
    .has_default = true,
    .label       = "mDNS enabled",
    .group       = "network",
};

static const bb_config_field_t s_update_check_enabled_field = {
    .id          = "update_check.enabled",
    .type        = BB_CONFIG_BOOL,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_SETTINGS_WIFI_NS, .key = BB_SETTINGS_UPDATE_CHECK_EN_KEY },
    .def         = { .b = true },
    .has_default = true,
    .label       = "Update check enabled",
    .group       = "system",
};

// Fail-open to the default (true) on a real backend error too, not just
// not-found -- mirrors bb_nv_config_init's own "nvs_get_u8(...) != ESP_OK ->
// default 1" fallback exactly (a storage error was treated the same as
// unset, never surfaced to the caller as false).
bool bb_settings_display_enabled_get(void)
{
    bool en = true;
    bb_err_t err = bb_config_get_bool(&s_display_enabled_field, &en);
    return err == BB_OK ? en : true;
}

bb_err_t bb_settings_display_enabled_set(bool en)
{
    return bb_config_set_bool(&s_display_enabled_field, en);
}

// Same fail-open-to-default rationale as bb_settings_display_enabled_get.
bool bb_settings_mdns_enabled_get(void)
{
    bool en = true;
    bb_err_t err = bb_config_get_bool(&s_mdns_enabled_field, &en);
    return err == BB_OK ? en : true;
}

bb_err_t bb_settings_mdns_enabled_set(bool en)
{
    return bb_config_set_bool(&s_mdns_enabled_field, en);
}

// Same fail-open-to-default rationale as bb_settings_display_enabled_get.
bool bb_settings_update_check_enabled_get(void)
{
    bool en = true;
    bb_err_t err = bb_config_get_bool(&s_update_check_enabled_field, &en);
    return err == BB_OK ? en : true;
}

bb_err_t bb_settings_update_check_enabled_set(bool en)
{
    return bb_config_set_bool(&s_update_check_enabled_field, en);
}

// ---------------------------------------------------------------------------
// RTC warm-reboot mirror (B1-763: crash-atomic since bb_storage_rtc gained
// the optional bb_storage txn vtable group). Shared by both live writers
// below (bb_settings_wifi_set and bb_settings_wifi_pending_promote).
// ---------------------------------------------------------------------------

// Distinct field descriptors, addr.backend="rtc" -- deliberately NOT
// s_wifi_ssid_field/s_wifi_pass_field above (those target backend="nvs";
// bb_config_staged_begin(&h, "rtc", NULL) followed by a stage against an
// "nvs"-addressed field would fail bb_config_staged's own cross-backend
// precheck). Capacity coupling note from the removed comment still applies:
// max_len here must stay byte-compatible with bb_storage_rtc_region_t's
// ssid[32]/pass[64] (components/bb_storage_rtc/include/bb_storage_rtc_region.h)
// -- not compiler-enforced on purpose, this component must gain NO direct
// bb_storage_rtc include/dependency (facade only, via "rtc" as a plain
// string). Editing either side, check the other.
// ns_or_dir="" (NOT NULL): bb_storage_txn_begin() rejects a NULL ns_or_dir
// outright (BB_ERR_INVALID_ARG, documented in bb_storage.h) even though
// bb_storage_rtc's get/set path ignores ns_or_dir entirely (mirroring
// "ram") -- an empty string satisfies the facade's non-NULL requirement
// while remaining a no-op value for this backend. Must match
// settings_wifi_rtc_mirror_write's bb_config_staged_begin() ns_or_dir arg
// exactly (bb_config_staged's precheck compares the two by content).
static const bb_config_field_t s_wifi_rtc_ssid_field = {
    .id      = "wifi.ssid_rtc_mirror",
    .type    = BB_CONFIG_STR,
    .addr    = { .backend = "rtc", .ns_or_dir = "", .key = "ssid" },
    .max_len = 32,
    .label   = "WiFi SSID (RTC mirror)",
    .group   = "network",
};

static const bb_config_field_t s_wifi_rtc_pass_field = {
    .id      = "wifi.pass_rtc_mirror",
    .type    = BB_CONFIG_STR,
    .addr    = { .backend = "rtc", .ns_or_dir = "", .key = "pass" },
    .max_len = 64,
    .label   = "WiFi Password (RTC mirror)",
    .group   = "network",
    .secret  = true,
};

static const bb_config_field_t s_wifi_rtc_provisioned_field = {
    .id    = "wifi.provisioned_rtc_mirror",
    .type  = BB_CONFIG_U8,
    .addr  = { .backend = "rtc", .ns_or_dir = "", .key = "provisioned" },
    .label = "WiFi provisioned flag (RTC mirror)",
    .group = "network",
};

// Best-effort mirror of the live ssid/pass into the "rtc" bb_storage backend
// (bb_storage_rtc's keyed contract: string keys "ssid"/"pass", uint8_t key
// "provisioned") -- a fast warm-reboot recovery cache, NOT the source of
// truth. NVS (via the bb_config_staged commit both in-file callers already
// ran, plus bb_nv's own NVS writes for its init-time mirror-seed/
// provisioned-flag-repack callers) is authoritative; every return here is
// ignored, including BB_ERR_NOT_FOUND from bb_config_staged_begin when no
// app has registered a "rtc" backend at all (fail-open: every caller must
// still return its own success regardless of this call's outcome).
//
// Single atomic bb_config_staged commit -- all 3 keys (ssid/pass/
// provisioned) land or none do, exactly like the live NVS writes above,
// since bb_storage_rtc now implements the optional bb_storage txn vtable
// group (B1-763). A crash/reset between begin() and commit() leaves the
// mirror at its PRIOR value, never a torn ssid/pass/provisioned mix -- a
// consumer may now trust provisioned==1 as "ssid/pass are a consistent
// pair" on this mirror.
//
// Public (declared in bb_settings.h) so bb_nv's init-time mirror-seed and
// bb_nv_config_set_provisioned's mirror repack (platform/espidf/bb_nv/
// bb_nv.c) can arm/refresh the mirror without a private bb_storage_rtc
// dependency of their own or duplicating this component's field
// descriptors/staging precheck -- see bb_settings.h's doc comment.
void bb_settings_wifi_rtc_mirror_write(const char *ssid, const char *pass)
{
    // NULL pass -> "" (open network), same asymmetry as bb_settings_wifi_set:
    // bb_config_staged_set_str POISONS the whole staged session on a literal
    // NULL value (never substitutes), so this substitution must happen here,
    // not be left to callers -- unlike this fn's original private-only
    // incarnation, it is now called directly with un-presanitized args (e.g.
    // bb_nv's mirror-seed/provisioned-repack).
    const char *p = pass ? pass : "";

    bb_config_staged_t h = {0};
    if (bb_config_staged_begin(&h, "rtc", "") != BB_OK) {
        return;  // fail-open -- no "rtc" backend registered, nothing to mirror.
    }
    bb_config_staged_set_str(&h, &s_wifi_rtc_ssid_field, ssid);
    bb_config_staged_set_str(&h, &s_wifi_rtc_pass_field, p);
    bb_config_staged_set_u8(&h, &s_wifi_rtc_provisioned_field, 1);
    (void)bb_config_staged_commit(&h);  // best-effort -- see comment above.
}

// ---------------------------------------------------------------------------
// WiFi live-creds writer (B1: bb_nv creds-cluster PR4). No validation --
// callers pre-validate ssid/pass (same posture as the pending-creds writers
// below).
// ---------------------------------------------------------------------------

// 2 keys, one atomic bb_config_staged commit -- all-or-nothing. See
// bb_settings.h for the full contract.
bb_err_t bb_settings_wifi_set(const char *ssid, const char *pass)
{
    const char *p = pass ? pass : "";

    bb_config_staged_t h = {0};
    bb_err_t err = bb_config_staged_begin(&h, "nvs", BB_SETTINGS_WIFI_NS);
    if (err != BB_OK) return err;

    bb_config_staged_set_str(&h, &s_wifi_ssid_field, ssid);
    bb_config_staged_set_str(&h, &s_wifi_pass_field, p);
    err = bb_config_staged_commit(&h);
    if (err != BB_OK) return err;

    // Best-effort -- see settings_wifi_rtc_mirror_write's own comment.
    bb_settings_wifi_rtc_mirror_write(ssid, p);
    return BB_OK;
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

// Pure storage read of the same try!=0 AND ssid-non-empty gate bb_wifi's
// bb_wifi_pending_decide implements (components/bb_wifi/bb_wifi_pending.c)
// -- reimplemented locally rather than depending on bb_wifi, since bb_settings
// is bb_nv's replacement surface here (a bb_settings -> bb_wifi dependency
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
// The relocated RTC warm-reboot mirror (bb_storage_rtc, "rtc" backend) is
// best-effort updated AFTER the atomic live-creds commit below succeeds --
// see settings_wifi_rtc_mirror_write's comment for the write-order/
// crash-atomicity rationale. Nothing calls promote on the live path yet
// (this PR is purely additive), so a stale/absent mirror isn't stranded by
// this behavior existing here.
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

    // Best-effort -- see settings_wifi_rtc_mirror_write's own comment.
    bb_settings_wifi_rtc_mirror_write(ssid, pass);

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

// ---------------------------------------------------------------------------
// RTC warm-reboot mirror accessors (bb_nv creds-cluster relocation) -- see
// bb_settings.h for the full contract of each.
// ---------------------------------------------------------------------------

bool bb_settings_wifi_rtc_mirror_has_creds(void)
{
    return bb_storage_exists(&s_wifi_rtc_ssid_field.addr);
}

// Same NULL-safe out_len guarantee as bb_settings_wifi_ssid_get. bb_config_get_str
// resolves an empty/invalid/unregistered mirror to BB_ERR_NOT_FOUND, which has
// no has_default on this field descriptor -- callers (bb_nv's heal) must check
// bb_settings_wifi_rtc_mirror_has_creds() first, mirroring how bb_nv's old
// heal gated on bb_storage_rtc_region_valid()+ssid[0]!='\0' before reading.
bb_err_t bb_settings_wifi_rtc_mirror_ssid_get(char *buf, size_t cap, size_t *out_len)
{
    size_t len = 0;
    return bb_config_get_str(&s_wifi_rtc_ssid_field, buf, cap, out_len ? out_len : &len);
}

// Never log the returned password value. Same contract as
// bb_settings_wifi_rtc_mirror_ssid_get.
bb_err_t bb_settings_wifi_rtc_mirror_pass_get(char *buf, size_t cap, size_t *out_len)
{
    size_t len = 0;
    return bb_config_get_str(&s_wifi_rtc_pass_field, buf, cap, out_len ? out_len : &len);
}

// Fail-CLOSED (false) on any backend error or an invalid/unregistered mirror
// -- deliberately the OPPOSITE default direction from
// bb_settings_display_enabled_get and friends, since this gates whether a
// heal re-marks a recovered board as provisioned; a storage error must never
// be silently treated as "yes, provisioned".
bool bb_settings_wifi_rtc_mirror_provisioned_get(void)
{
    uint8_t val = 0;
    bb_err_t err = bb_config_get_u8(&s_wifi_rtc_provisioned_field, &val);
    return err == BB_OK && val != 0;
}

// Whole-region invalidate (rtc_erase ignores which of ssid/pass/provisioned
// is passed -- any of the three invalidates the entire mirror region, see
// bb_storage_rtc). Returns bb_storage_erase's own result, including
// BB_ERR_NOT_FOUND when no "rtc" backend is registered -- callers that want
// fail-open here (bb_nv's factory-reset/clear paths) ignore the return, same
// posture as this component's own write paths.
bb_err_t bb_settings_wifi_rtc_mirror_clear(void)
{
    return bb_storage_erase(&s_wifi_rtc_ssid_field.addr);
}

// ---------------------------------------------------------------------------
// Creds-boot heal/seed shell + /api/manifest registration (B1-963/B1-708:
// relocated VERBATIM from platform/espidf/bb_nv/bb_nv.c's
// bb_nv_config_init()/bb_nv_config_manifest_init() -- see bb_settings.h for
// the public contract of each).
// ---------------------------------------------------------------------------

// bb_config_type_t -> schema type string, parallel to bb_config_type_to_enc
// (components/bb_config) -- pure, stateless, no storage access. Only the
// types s_overlay_fields below actually uses (BOOL, STR) are distinguished;
// anything else defaults to "str" -- widen this (mirroring the fuller
// "str"|"u8"|"u16"|"u32"|"i32"|"blob"|"bool" vocabulary the old
// bb_manifest_nv_t.type hand-typed literals used) if/when the overlay's
// field scope grows to cover a scalar-numeric field.
static const char *bb_settings_config_type_to_str(bb_config_type_t t)
{
    switch (t) {
    case BB_CONFIG_BOOL:
        return "bool";
    case BB_CONFIG_STR:
    default:
        return "str";
    }
}

// The bb_config_field_t literals this overlay covers -- same 3 live "bb_cfg"
// keys the old s_creds_boot_manifest_keys[] enumerated (wifi_ssid/wifi_pass/
// provisioned), NOT the pending-creds or RTC-mirror fields (least-surprise
// scope match, see bb_settings.h). Pointers into the `static const`
// bb_config_field_t literals declared above -- read-only, nothing here
// mutates addr/key/max_len.
static const bb_config_field_t *const s_overlay_fields[] = {
    &s_wifi_ssid_field,
    &s_wifi_pass_field,
    &s_wifi_provisioned_field,
};

size_t bb_settings_nv_overlay_entries(bb_settings_nv_overlay_entry_t *out, size_t cap)
{
    size_t total = sizeof(s_overlay_fields) / sizeof(s_overlay_fields[0]);
    size_t n = (out == NULL) ? 0 : (cap < total ? cap : total);

    for (size_t i = 0; i < n; i++) {
        const bb_config_field_t *f = s_overlay_fields[i];
        out[i].ns_or_dir         = f->addr.ns_or_dir;
        out[i].key               = f->addr.key;
        out[i].type_str          = bb_settings_config_type_to_str(f->type);
        out[i].label             = f->label;
        out[i].secret            = f->secret;
        out[i].provisioning_only = f->provisioning_only;
        out[i].reboot_required   = f->reboot_required;
    }

    return total;
}

// Moved verbatim from bb_nv_config_init (platform/espidf/bb_nv/bb_nv.c) --
// ESP-IDF only (host stub below returns BB_OK immediately, matching the
// original's #ifdef ESP_PLATFORM-wrapped body exactly: on host this was
// always a pure no-op).
bb_err_t bb_settings_creds_boot_init(void)
{
#ifdef ESP_PLATFORM
    bb_err_t flash_err = bb_storage_nvs_flash_init();
    if (flash_err != BB_OK) return flash_err;

#if defined(CONFIG_BB_SETTINGS_CREDS_RTC_BACKUP)
    /* The heal-vs-seed DECISION is a pure function
     * (bb_settings_creds_boot_decide, components/bb_settings/src/) and is
     * host-tested there -- see test/test_host/test_bb_settings_creds_boot_decide.c
     * for all 4 input combinations plus bite-proof (RED-when-inverted)
     * evidence. Everything below this point is the NVS/RTC-mirror I/O for
     * whichever action came back; that I/O is espidf-only (B1-943/B1-516,
     * coverage-invisible) and rides on HW validation, NOT host coverage --
     * do not read the `make coverage` gate as proof this I/O is exercised.
     *
     * requires=storage_rtc on this fn's // bbtool:init marker (bb_settings.h)
     * forces bb_storage_rtc's registration to run first in the same EARLY
     * tier -- without it, bb_settings_wifi_rtc_mirror_has_creds() would
     * silently read BB_ERR_NOT_FOUND/false and the heal action would never
     * be decided. */
    bb_settings_creds_boot_action_t action =
        bb_settings_creds_boot_decide(bb_settings_wifi_has_creds(),
                                       bb_settings_wifi_rtc_mirror_has_creds());

    if (action == BB_SETTINGS_CREDS_BOOT_HEAL) {
        /* Restore: NVS has no live creds but the shared "rtc" mirror
         * (bb_settings_wifi_rtc_mirror_*, "rtc" bb_storage backend) does --
         * recover them by writing straight through bb_settings' live-creds
         * writer -- a single atomic bb_config_staged commit against the SAME
         * "bb_cfg"/wifi_ssid/wifi_pass NVS keys bb_nv used to write directly
         * (byte-compat unaffected by this relocation, see bb_settings.h). */
        char ssid[32] = {0};
        char pass[64] = {0};
        size_t ssid_len = 0, pass_len = 0;
        bb_err_t sret = bb_settings_wifi_rtc_mirror_ssid_get(ssid, sizeof(ssid), &ssid_len);
        bb_err_t pret = bb_settings_wifi_rtc_mirror_pass_get(pass, sizeof(pass), &pass_len);
        if (sret == BB_OK && pret == BB_OK && ssid[0] != '\0') {
            bb_err_t werr = bb_settings_wifi_set(ssid, pass);
            if (werr == BB_OK) {
                bb_log_w(TAG, "creds restored from RTC backup");
                if (bb_settings_wifi_rtc_mirror_provisioned_get()) {
                    /* Re-stamp the live "provisioned" NVS flag + RTC mirror
                     * -- moved verbatim from bb_nv_config_set_provisioned's
                     * body (its ONLY caller was this heal branch; it is now
                     * inlined here rather than kept as a cross-component call
                     * back into bb_nv, so this shell has zero remaining
                     * bb_nv dependency). Byte-compat: BB_CONFIG_BOOL encodes
                     * as nvs_set_u8 under the hood, identical to the
                     * original's direct nvs_set_u8(handle, "provisioned", 1)
                     * call. */
                    bb_err_t perr = bb_config_set_bool(&s_wifi_provisioned_field, true);
                    if (perr == BB_OK) {
                        /* ssid/pass here still equal the live creds just
                         * written by bb_settings_wifi_set() above -- no
                         * fresh live re-read needed. A future reorder of
                         * this HEAL sequence must preserve that invariant. */
                        bb_settings_wifi_rtc_mirror_write(ssid, pass);
                    } else {
                        bb_log_e(TAG, "provisioned re-stamp after RTC restore failed: %d", (int)perr);
                    }
                }
            } else {
                bb_log_e(TAG, "creds restore from RTC backup failed to persist: %d", (int)werr);
            }
        }
    } else if (action == BB_SETTINGS_CREDS_BOOT_SEED) {
        /* Mirror-seed: proactively arm the recovery net on a freshly-flashed
         * provisioned board, rather than lazily on the next credential
         * write. Never overwrites an already-valid mirror (which may hold
         * in-flight pending-try state written by
         * bb_settings_wifi_pending_promote), and is idempotent across warm
         * resets (a valid mirror decides NONE on every subsequent boot; this
         * fires at most once, right after a cold boot with erased/never-
         * armed RTC memory). */
        char ssid[32] = {0};
        char pass[64] = {0};
        size_t ssid_len = 0, pass_len = 0;
        bb_err_t sret = bb_settings_wifi_ssid_get(ssid, sizeof(ssid), &ssid_len);
        bb_err_t pret = bb_settings_wifi_pass_get(pass, sizeof(pass), &pass_len);
        if (sret == BB_OK && pret == BB_OK) {
            bb_settings_wifi_rtc_mirror_write(ssid, pass);
        }
    }
#endif

    bb_log_i(TAG, "config loaded");
#endif
    return BB_OK;
}

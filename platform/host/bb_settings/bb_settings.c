// bb_settings — default WiFi-credentials store backed by bb_config.
//
// Portable (no ESP-IDF deps): compiled on host and ESP-IDF, mirroring
// bb_config and bb_dispatch_cmd. The field table below targets the SAME
// NVS namespace/keys bb_nv_config already uses ("bb_cfg"/"wifi_ssid"/
// "wifi_pass") -- see components/bb_settings/include/bb_settings.h for the
// byte-compat rationale.

#include "bb_settings.h"
#include "bb_config.h"

// Namespace/keys/max-lengths byte-for-byte matched to
// platform/espidf/bb_nv/bb_nv.c's BB_NV_KEY_WIFI_SSID/BB_NV_KEY_WIFI_PASS
// under BB_NV_CONFIG_NVS_NS ("bb_cfg") -- do not change without a migration
// plan, this strands provisioned-board credentials otherwise.
#define BB_SETTINGS_WIFI_NS       "bb_cfg"
#define BB_SETTINGS_WIFI_SSID_KEY "wifi_ssid"
#define BB_SETTINGS_WIFI_PASS_KEY "wifi_pass"

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

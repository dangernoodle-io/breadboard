// bb_settings — default bb_wifi_creds_provider_t backed by bb_config.
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

static bb_err_t settings_get_ssid(void *ctx, char *buf, size_t cap, size_t *out_len)
{
    (void)ctx;
    return bb_config_get_str(&s_wifi_ssid_field, buf, cap, out_len);
}

// Never log the returned password value -- secret=true on the field
// descriptor above documents this; callers must honor it too.
static bb_err_t settings_get_pass(void *ctx, char *buf, size_t cap, size_t *out_len)
{
    (void)ctx;
    return bb_config_get_str(&s_wifi_pass_field, buf, cap, out_len);
}

// Non-empty-value semantics -- matches bb_wifi's fallback wifi_has_creds()
// (ssid[0] != '\0'), NOT mere key presence. A cap=0 call is a valid
// bb_config_get_str size-probe (returns the true length via out_len without
// touching buf); an empty-but-present ssid key probes to out_len==0 and
// correctly reports "no creds", keeping provider and fallback zero-drift.
static bool settings_has_creds(void *ctx)
{
    (void)ctx;
    size_t len = 0;
    bb_err_t err = bb_config_get_str(&s_wifi_ssid_field, NULL, 0, &len);
    return err == BB_OK && len > 0;
}

static bb_err_t settings_clear(void *ctx)
{
    (void)ctx;
    bb_err_t err = bb_config_erase(&s_wifi_ssid_field);
    bb_err_t err2 = bb_config_erase(&s_wifi_pass_field);
    return (err != BB_OK) ? err : err2;
}

static const bb_wifi_creds_provider_t s_wifi_creds_provider = {
    .get_ssid  = settings_get_ssid,
    .get_pass  = settings_get_pass,
    .has_creds = settings_has_creds,
    .clear     = settings_clear,
};

const bb_wifi_creds_provider_t *bb_settings_wifi_creds_provider(void)
{
    return &s_wifi_creds_provider;
}

void *bb_settings_wifi_creds_ctx(void)
{
    return NULL;
}

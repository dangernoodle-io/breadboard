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

#ifdef __cplusplus
}
#endif

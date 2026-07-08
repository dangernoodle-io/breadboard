#pragma once

// bb_settings — bb's default bb_wifi_creds_provider_t, backed by bb_config.
//
// PR1 scope: the wifi-creds field table + a default provider that forwards
// to bb_config_get_str/set_str/exists/erase over the SAME NVS namespace/keys
// bb_nv_config already uses ("bb_cfg"/"wifi_ssid"/"wifi_pass") — byte-compat
// with provisioned boards because bb_config's STR encoding routes through
// bb_storage's "nvs" backend get_typed/set_typed, which calls nvs_get_str/
// nvs_set_str under the hood (see bb_storage_nvs).
//
// bb_settings is OPTIONAL (KB 795 — un-opinionated). A consumer composes it
// to get bb's default wifi-creds provider, OR supplies its own
// bb_wifi_creds_provider_t and omits bb_settings entirely. Nothing here
// self-registers.
//
// DEFERRED to later PRs (not in scope here): NVS lifecycle (factory-reset/
// boot-count/pending-creds) and bb_manifest dissolution.

#include "bb_wifi_creds.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns bb's default wifi-creds provider vtable (stateless, always valid).
const bb_wifi_creds_provider_t *bb_settings_wifi_creds_provider(void);

// Returns the ctx to pass alongside bb_settings_wifi_creds_provider() to
// every vtable call. The default provider is stateless (forwards straight
// to bb_config field accessors) so this is always NULL today — callers
// should still pass it through rather than assuming NULL, in case a future
// provider needs instance state.
void *bb_settings_wifi_creds_ctx(void);

#ifdef __cplusplus
}
#endif

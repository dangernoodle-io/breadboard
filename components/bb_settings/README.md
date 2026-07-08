# bb_settings

bb's default WiFi-credentials store — a wifi-creds field table over `bb_config`, byte-compatible with the credentials `bb_nv_config` already persists. `bb_settings` is bb's opinionated bb-config authority (KB 805/806); `bb_wifi` reads its accessors directly.

## When to use / when not

Use it when a consumer wants bb's ready-made wifi-creds store without hand-rolling its own `bb_config_field_t` table. Compose it (add `REQUIRES bb_settings`) — `bb_wifi` reads `bb_settings_wifi_ssid_get`/`bb_settings_wifi_pass_get`/`bb_settings_wifi_has_creds` directly for the live-creds connect path.

**Scope note:** this component ships the field table + direct accessors only. NVS lifecycle (factory-reset/boot-count/pending-creds) and `bb_manifest` dissolution are deferred to later PRs.

## Public API

See [`include/bb_settings.h`](include/bb_settings.h). Key symbols: `bb_settings_wifi_ssid_get()`, `bb_settings_wifi_pass_get()`, `bb_settings_wifi_has_creds()`.
Public symbols in this component use the `bb_` prefix.

## Config knobs

None — this component has no Kconfig options. The wifi SSID/password NVS namespace and keys are fixed constants (`"bb_cfg"`/`"wifi_ssid"`/`"wifi_pass"`) matched byte-for-byte to `bb_nv_config`'s existing storage, so provisioned-board credentials remain readable after adopting this provider.

## Dependencies

<!-- BEGIN bbtool:deps -->
**REQUIRES:** `bb_config`, `bb_core`

**PRIV_REQUIRES:** _(none)_
<!-- END bbtool:deps -->

## Platform support

<!-- BEGIN bbtool:platform -->
| host | espidf | arduino |
|------|--------|---------|
| yes | no | no |
<!-- END bbtool:platform -->

## Links

[![build](https://github.com/dangernoodle-io/breadboard/actions/workflows/build.yml/badge.svg)](https://github.com/dangernoodle-io/breadboard) [![coverage](https://coveralls.io/repos/github/dangernoodle-io/breadboard/badge.svg?branch=main)](https://github.com/dangernoodle-io/breadboard)

- Repository: [https://github.com/dangernoodle-io/breadboard](https://github.com/dangernoodle-io/breadboard)
- Wiki: [https://github.com/dangernoodle-io/breadboard/wiki](https://github.com/dangernoodle-io/breadboard/wiki)

## See also

`bb_wifi` — the sole production consumer of the accessors above. `bb_config`/`bb_storage` — the typed configuration facade the accessors forward to. `bb_nv_config` — the legacy wifi-creds storage this component's field table targets byte-compatibly (same NVS namespace/keys, same on-flash string encoding).

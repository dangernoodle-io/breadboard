# bb_wifi_creds

Interface-only component defining the wifi-credential-provider seam (`bb_wifi_creds_provider_t`).

## When to use / when not

Use it when a component that consumes wifi credentials (e.g. a future `bb_wifi`) wants to depend on an abstract provider rather than a specific storage backend. A provider implementation (e.g. `bb_settings`, or a consumer's own `tm_settings`) implements the vtable; the consumer takes a `const bb_wifi_creds_provider_t *` + `void *ctx` by explicit injection — never a global lookup, never self-registration.

Don't use it if your app hard-wires a single wifi-creds source and has no need for the abstraction — calling `bb_config`/`bb_nv_config` directly is simpler.

## Public API

See [`include/bb_wifi_creds.h`](include/bb_wifi_creds.h). Key symbol: `bb_wifi_creds_provider_t` — `{get_ssid, get_pass, has_creds, clear}`. `get_ssid`/`get_pass` mirror `bb_config_get_str`'s size-probe/truncation contract (`cap=0` probes length). The password value is secret — implementations and callers must never log it.
Public symbols in this component use the `bb_` prefix.

## Config knobs

None — this component is a header-only interface, no Kconfig options.

## Dependencies

<!-- BEGIN bbtool:deps -->
**REQUIRES:** `bb_core`

**PRIV_REQUIRES:** _(none)_
<!-- END bbtool:deps -->

## Platform support

<!-- BEGIN bbtool:platform -->
| host | espidf | arduino |
|------|--------|---------|
| no | no | no |
<!-- END bbtool:platform -->

## Links

[![build](https://github.com/dangernoodle-io/breadboard/actions/workflows/build.yml/badge.svg)](https://github.com/dangernoodle-io/breadboard) [![coverage](https://coveralls.io/repos/github/dangernoodle-io/breadboard/badge.svg?branch=main)](https://github.com/dangernoodle-io/breadboard)

- Repository: [https://github.com/dangernoodle-io/breadboard](https://github.com/dangernoodle-io/breadboard)
- Wiki: [https://github.com/dangernoodle-io/breadboard/wiki](https://github.com/dangernoodle-io/breadboard/wiki)

## See also

`bb_settings` — bb's default provider implementation, backed by `bb_config`/`bb_storage`. This component intentionally has no implementation and no test — it exists solely as the shared vtable shape.

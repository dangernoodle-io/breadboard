# bb_wifi_creds

Interface component defining the wifi-credential-provider seam (`bb_wifi_creds_provider_t`), plus one pure, host-testable dispatch helper (`bb_wifi_creds_read`) used by `bb_wifi` to route provider-vs-fallback reads through a single call site.

## When to use / when not

Use it when a component that consumes wifi credentials (e.g. a future `bb_wifi`) wants to depend on an abstract provider rather than a specific storage backend. A provider implementation (e.g. `bb_settings`, or a consumer's own `tm_settings`) implements the vtable; the consumer takes a `const bb_wifi_creds_provider_t *` + `void *ctx` by explicit injection — never a global lookup, never self-registration.

Don't use it if your app hard-wires a single wifi-creds source and has no need for the abstraction — calling `bb_config`/`bb_nv_config` directly is simpler.

## Public API

See [`include/bb_wifi_creds.h`](include/bb_wifi_creds.h). Key symbol: `bb_wifi_creds_provider_t` — `{get_ssid, get_pass, has_creds, clear}`. `get_ssid`/`get_pass` mirror `bb_config_get_str`'s size-probe/truncation contract (`cap=0` probes length). The password value is secret — implementations and callers must never log it.

`bb_wifi_creds_read(provider_fn, pctx, fallback_fn, fctx, buf, cap, out_len)` — pure dispatcher: calls `provider_fn` if non-NULL, else `fallback_fn`. Always passes a non-NULL `out_len` to whichever function it calls (a local, when the caller's own `out_len` is NULL), so a provider relying on `bb_config_get_str`'s contract (which rejects `out_len==NULL`) always gets a valid pointer and actually populates `buf`. `bb_wifi`'s `wifi_read_ssid`/`wifi_read_pass` are the sole production callers.

Public symbols in this component use the `bb_` prefix.

## Config knobs

None — no Kconfig options.

## Dependencies

<!-- BEGIN bbtool:deps -->
**REQUIRES:** `bb_core`

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

`bb_settings` — bb's default provider implementation, backed by `bb_config`/`bb_storage`. This component's own implementation is limited to the pure `bb_wifi_creds_read` dispatcher above — no provider vtable implementation lives here.

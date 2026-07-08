# bb_config

Typed configuration layer over `bb_storage` — gives its blob-only vtable scalar-typed meaning (bool/u8/u16/u32/i32/str/blob) via a caller-owned field descriptor table.

## When to use / when not

Use it when a consumer wants named, typed config fields (e.g. `wifi.ssid`, `mqtt.enabled`) backed by any `bb_storage` backend, with an optional default value returned on first read. Don't use it for high-frequency data — each accessor call round-trips through `bb_storage`'s get/set, with no caching layer of its own.

## Public API

See [`include/bb_config.h`](include/bb_config.h). Key symbols: `bb_config_type_t`, `bb_config_field_t`, `bb_config_get_{bool,u8,u16,u32,i32,str,blob}`, `bb_config_set_{bool,u8,u16,u32,i32,str,blob}`, `bb_config_erase()`, `bb_config_exists()`.
Public symbols in this component use the `bb_` prefix.

## Composition-only, no registry

A field is declared as a `static const bb_config_field_t` naming its `id`, `type`, and `bb_storage_addr_t addr` (which backend/namespace/key it targets); accessors resolve `addr` against `bb_storage` on every call. There is no global registry and no init function — nothing here self-registers, so this component stays composition-only by design (see the DI legacy fence in breadboard/CLAUDE.md). A later schema-endpoint consumer may add an opt-in enumeration registry as a separate component; it will not change this header.

Scalars are byte-encoded via `bb_core`'s `bb_byte_order` helpers (fixed-width little-endian); `bool` is a single `0x00`/`0x01` byte. STR/BLOB accessors mirror `bb_storage_get`'s truncation/size-probe contract exactly (`cap=0` probes length; `*out_len` always reports the full stored length regardless of `cap`).

**Concurrency contract:** inherited entirely from whichever `bb_storage` backend a field's `addr` targets (e.g. NVS serializes internally). `bb_config` adds no per-field lock of its own — a consumer needing atomic read-modify-write across multiple fields must arrange its own synchronization.

## Config knobs

None — this component has no Kconfig options and no compile-time capacity knobs.

## Dependencies

<!-- BEGIN bbtool:deps -->
**REQUIRES:** `bb_core`, `bb_storage`

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

`bb_storage` — the underlying facade this component types; `bb_storage_ram` — the in-memory reference backend used by this component's host tests. Render metadata fields (`label`/`help`/`group`/`secret`/`provisioning_only`/`reboot_required`) on `bb_config_field_t` are carried but unconsumed in this PR — reserved for a future schema-endpoint consumer (mini-OpenAPI over config fields).

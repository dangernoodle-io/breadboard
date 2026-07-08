# bb_storage_ram

In-memory `bb_storage` backend — the reference implementation, a fixed-capacity key/value table with no heap allocation.

## When to use / when not

Use it for tests, host builds, or any consumer that wants `bb_storage` semantics without a persistent medium (e.g. a scratch cache, or a stand-in while a real backend is being bootstrapped). Don't use it where values must survive a reboot — this backend is pure RAM, nothing is ever written to flash or disk.

## Public API

See [`include/bb_storage_ram.h`](include/bb_storage_ram.h). Key symbols: `bb_storage_ram_register()`, `bb_storage_ram_test_reset()`.
Public symbols in this component use the `bb_` prefix.

## Registration

Composition-only, mirroring `bb_storage`'s rule: call `bb_storage_ram_register()` explicitly to wire this backend in under the name `"ram"`. No self-registration, no `AUTOREGISTER` Kconfig.

## Thread safety

The in-memory table is guarded by a mutex (mirroring the `bb_power`/`bb_fan` poll/snapshot pattern) — `get`/`set`/`erase`/`exists` are safe to call concurrently from multiple tasks.

## Config knobs

- `BB_STORAGE_RAM_MAX_ENTRIES` (default 32, Kconfig `CONFIG_BB_STORAGE_RAM_MAX_ENTRIES`) — table capacity. A new key once full returns `BB_ERR_NO_SPACE`.
- `BB_STORAGE_RAM_MAX_VALUE_BYTES` (default 256, Kconfig `CONFIG_BB_STORAGE_RAM_MAX_VALUE_BYTES`) — max value size. A larger `bb_storage_set()` returns `BB_ERR_NO_SPACE` (never truncated).
- `BB_STORAGE_RAM_MAX_KEY_BYTES` (default 64, Kconfig `CONFIG_BB_STORAGE_RAM_MAX_KEY_BYTES`) — max key length including the NUL terminator. A key at or above this length returns `BB_ERR_INVALID_ARG`.

## Dependencies

<!-- BEGIN bbtool:deps -->
**REQUIRES:** `bb_core`

**PRIV_REQUIRES:** `bb_log`, `bb_storage`, `bb_str`
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

`bb_storage` — the facade this backend plugs into.

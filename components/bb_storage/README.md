# bb_storage

Portable storage facade + backend registry: one `bb_storage_get/set/erase/exists` API dispatching by `bb_storage_addr_t.backend` to whichever backend has registered itself.

## When to use / when not

Use it whenever a component needs to read/write a value by logical address without hard-coding which storage medium (NVS, SD card, RAM, ...) backs it — register a backend once at composition time and every call site stays medium-agnostic. Don't use it for high-frequency/hot-path data with tight latency budgets on a backend with slow I/O (e.g. flash NVS) — those constraints belong to the backend, not the facade, but the facade adds no buffering or caching of its own.

## Public API

See [`include/bb_storage.h`](include/bb_storage.h). Key symbols: `bb_storage_addr_t`, `bb_storage_vtable_t`, `bb_storage_register_backend()`, `bb_storage_get()`, `bb_storage_set()`, `bb_storage_erase()`, `bb_storage_exists()`.
Public symbols in this component use the `bb_` prefix.

## Composition-only

`bb_storage` never self-registers a backend. Application composition code (or test setup) calls `bb_storage_register_backend(name, vtable, impl)` explicitly for each backend it wants wired up — no `AUTOREGISTER` Kconfig, no constructor. See breadboard/CLAUDE.md's DI legacy fence: new components stay composition-only by design.

**Concurrency contract:** backends are registered once at composition/init time — a single writer, never concurrent with other registrations or with `get`/`set`/`erase`/`exists` dispatch. `bb_storage_register_backend()` itself is **not** concurrency-safe. Once composition has finished, per-call `get`/`set`/`erase`/`exists` dispatch is safe to call concurrently from multiple tasks; each backend is responsible for its own internal locking (e.g. `bb_storage_ram` guards its table with a mutex).

## Config knobs

`BB_STORAGE_MAX_BACKENDS` (default 4, Kconfig `CONFIG_BB_STORAGE_MAX_BACKENDS`) — backend registry capacity. Registering more backends than this returns `BB_ERR_NO_SPACE`.

## Dependencies

<!-- BEGIN bbtool:deps -->
**REQUIRES:** `bb_core`

**PRIV_REQUIRES:** `bb_log`
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

`bb_storage_ram` — the in-memory reference backend, registers itself as `"ram"` when the consumer calls `bb_storage_ram_register()`. Additional backends (NVS, SD card) are later PRs layered on top of this facade; unknown `addr->backend` values return `BB_ERR_NOT_FOUND`, never a crash.

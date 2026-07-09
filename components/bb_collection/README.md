# bb_collection

A humble, fixed-capacity, thread-safe ordered collection of caller-owned opaque items.

## When to use / when not

Use it when a consumer wants a simple named+ordered store — append entries, then iterate them back out sorted by a construction-time `order` field. Don't use it as a lookup-by-name/type resolver: it has no `get`/`find` API and is not an autowire mechanism. For name/pointer -> value lookup, use `bb_registry`; for type-based resolution, that's a future `bb_autowire` (governed separately — see the header doc).

## Public API

See [`include/bb_collection.h`](include/bb_collection.h). Key symbols: `bb_collection_entry_t`, `bb_collection_t`, `BB_COLLECTION_DEFINE()`, `bb_collection_add()`, `bb_collection_foreach()`, `bb_collection_count()`.

A generated C API reference site (Doxygen+MkDocs) is future work — tracked as B1-348.

## Config knobs

`BB_COLLECTION_SNAPSHOT_MAX` (fixed at 64, not a Kconfig knob) — the stack-buffer bound `bb_collection_foreach()` snapshots into. All `BB_COLLECTION_DEFINE()` capacities must be `<= BB_COLLECTION_SNAPSHOT_MAX` (enforced by `_Static_assert`).

## Dependencies

<!-- BEGIN bbtool:deps -->
| Component | Kind | Role | Docs |
|-----------|------|------|------|
| `bb_core` | public | Foundational, near-zero-dep primitives every bb_* component builds on: the portable error type, the canonical clock, run-exactly-once, a contention-instrumented lock, byte-order helpers, memory accounting, and the reboot-reason codec. | [bb_core](../bb_core/README.md) |
| `bb_log` | private | — | [bb_log](../README.md) |
<!-- END bbtool:deps -->

## Platform support

<!-- BEGIN bbtool:platform -->
| host | espidf | arduino |
|------|--------|---------|
| yes | no | no |
<!-- END bbtool:platform -->

`bb_collection`'s single implementation (`platform/host/bb_collection/bb_collection.c`) compiles unchanged on host and ESP-IDF via `pthread_mutex_t` (same primitive `bb_registry` uses) — the table above reflects directory presence, not a host-only restriction.

## See also

`bb_registry` — name/pointer-keyed lookup table; reach for that instead if a consumer needs `get`-by-key rather than ordered enumeration. `bb_attrs`/`bb_filter` — priority/kind/tag projection over caller-supplied arrays; a different shape (stateless one-shot select vs. a stored, ordered collection).

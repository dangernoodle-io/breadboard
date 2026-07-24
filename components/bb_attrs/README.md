# bb_attrs

Intrusive header carrying filter/collection metadata (`priority`, `kind`, `tag_mask`, `delivery_class`) that any element embeds as a member. Reach for it when a set of heterogeneous elements needs to be projected/selected by `bb_filter` without a shared base type or heap-allocated wrapper.

## When to use / when not

Use it whenever an element needs to be filterable by priority/kind/tag/delivery class (pair with `bb_filter`). Don't use it as a general object system or registry key — there is no `type_id` and no ownership/lifecycle semantics here (YAGNI: add `type_id` only when a real heterogeneous collection needs runtime type recovery).

## Public API

See [`include/bb_attrs.h`](include/bb_attrs.h). Key symbols: `bb_attrs_t`, `BB_ATTRS_DELIVERY_MUST` / `BB_ATTRS_DELIVERY_DEFERRABLE`, `bb_attrs_container_of()`.

A generated C API reference site (Doxygen+MkDocs) is future work — tracked as B1-348.

## Config knobs

None.

## Dependencies

<!-- BEGIN bbtool:deps -->
| Component | Kind | Role | Docs |
|-----------|------|------|------|
| `bb_core` | public | Foundational, near-zero-dep primitives every bb_* component builds on: the portable error type, the canonical clock, run-exactly-once, a contention-instrumented lock, byte-order helpers, memory accounting, and the reboot-reason codec. | [bb_core](../bb_core/README.md) |
<!-- END bbtool:deps -->

## Platform support

<!-- BEGIN bbtool:platform -->
| host | espidf | arduino |
|------|--------|---------|
| yes | no | no |
<!-- END bbtool:platform -->

`bb_attrs` is a portable intrusive header with no platform-specific implementation
directory — the table above reflects `platform/<plat>/bb_attrs/` directory presence
OR host unit tests under `test/test_host/`, not language portability (it compiles
unchanged everywhere).

## See also

`bb_filter` — the pure projection component that reads `bb_attrs_t` off elements. `bb_collection` is a coming consumer (not yet built).

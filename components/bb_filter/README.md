# bb_filter

Pure projection over elements carrying `bb_attrs`: given an array of `{attrs, item}` pairs and a selector, returns the matching, priority-sorted subset. Reach for it whenever a consumer needs to pick "the top N by priority/kind/tag" out of a candidate set without building a registry.

## When to use / when not

Use it for one-shot or per-tick selection over a caller-owned array. Don't use it as a registry or iterator over live state — it never owns, dereferences `item`, or retains anything between calls; each call is independent and stateless.

## Public API

See `include/bb_filter.h`. Key symbols: `bb_filter_elem_t`, `bb_filter_selector_t`, `bb_filter_select()`, `bb_filter_emit_t`, `bb_filter_emit_decide()`.

## Config knobs

`BB_FILTER_PRESSURE_MS_PER_UNIT` (default 100) — ms of cadence floor per unit of `sel->pressure` in `bb_filter_emit_decide()`. Override via `-D` if needed.

## Dependencies

REQUIRES `bb_core`, `bb_attrs`. No PRIV_REQUIRES.

## Platform support

| host | espidf | arduino |
|------|--------|---------|
| full | full   | n/a (portable, no platform code) |

## See also

`bb_attrs` — the intrusive header this component reads. `bb_collection` and `bb_sink_display` are the coming consumers (not yet built).

**Pressure/delivery-class adaptive shed.** `bb_filter_select()` sheds DEFERRABLE elements under pressure before ever touching MUST elements (MUST is never shed by pressure, only ever deferred by `bb_filter_emit_decide()`). `sel->pressure` is a bare input parameter today — nothing wires a live signal (e.g. `bb_transport_health`) into it. That derivation is a documented, deliberately deferred future seam; see the header comment above `bb_filter_selector_t`.

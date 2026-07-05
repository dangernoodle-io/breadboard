# bb_sink_display

The northstar display-egress sink: renders a board-appropriate subset of
bb_cache-backed values onto a display, change-driven, with no per-app render
code required for the common case.

## When to use / when not

Use it when a board has a display and you want live bb_cache values shown on
it -- a fixed set of rows for a small OLED, or a rich LVGL screen on a bigger
panel. Don't reach for it to build a general-purpose UI framework: v1 is a
fixed field catalog + two render policies (generic text rows, or hand the
data to your own render function), not a widget toolkit.

## v1 scope (locked)

- Field catalog: a **hand-authored static `bb_sink_display_field_t[]`**
  (in the espidf glue file), not a dynamic contribution registry.
  A future swap replaces this array with a `bb_collection`-fed satellite
  contribution behind a one-line `#if` -- the descriptor shape here does not
  change.
- Selection: `bb_filter_select()` over that array, gated by the board's
  `bb_sink_display_caps_t` (screen tier / field count / supported kinds).
- Delivery: one `bb_cache_reactive` observer per distinct selected
  `cache_key` (never observe-all), coalesced into a single deferred
  `bb_timer` periodic redraw tick (`cfg->rate_limit_ms`, runs on the shared
  `bb_timer_disp` task per the Timer callback convention -- never the
  esp_timer service task, since it makes blocking `bb_display_*`/`cfg->custom`
  calls) that does minimal-redraw (only rows that changed) and sweeps ts_ms
  freshness (stale -> greyed, evicted -> dropped). `cfg->evict_after_ms` must
  be `> cfg->stale_after_ms` (rejected by `bb_sink_display_init()` otherwise)
  -- a row can't be evicted before/at the same age it's marked stale.
- No `bb_collection` dependency yet (that lands separately; see the phasing
  note in `include/bb_sink_display.h`).

## "Inject the abstraction, not the driver"

`bb_display` has no instantiable handle type -- it's a link-time-selected
singleton (the board picks its `bb_display_<chip>` backend; `bb_display`
probes and selects one). So v1's two injection points are:

- **DEFAULT_LINES**: calls the `bb_display` singleton's free functions
  (`bb_display_draw_text`/`_width`/`_height`) directly. The board's backend
  choice is the injection, resolved at link time -- never a runtime handle.
- **CUSTOM**: hands the selected + formatted row subset to `cfg->custom`.
  The app owns its own rich UI (e.g. an LVGL `lv_subject` bridge) and this
  component never touches `bb_display` in that mode.

`bb_sink_display` **never links LVGL**, in either policy.

## Public API

See [include/bb_sink_display.h](include/bb_sink_display.h) for the field
descriptor, caps/config structs, `bb_sink_display_init()`/`_start()`, and the
pure building blocks (caps->selector, `bb_filter`-backed selection, default
formatter, value resolver, config validation, and the row-table family). Full
symbol reference: the generated C API docs (Doxygen+MkDocs, B1-348) — until
published, see the header.

## Config knobs

`BB_SINK_DISPLAY_MAX_FIELDS` (default 16) — fixed capacity for the field
catalog, the `bb_filter_select()` output, and the row table.
`BB_SINK_DISPLAY_VALUE_MAX` (default 40) — formatted row value buffer size.

## Reserved multi-display seam

`bb_sink_display_config_t.display` is `NULL` in every real v1 consumer
(single-display boards only). It exists so a future multi-display refactor
of `bb_display` has a non-breaking place to plug in; `bb_sink_display_init()`
rejects a non-NULL value today with `BB_ERR_UNSUPPORTED` (shape-now,
wire-later -- same pattern as `bb_filter`'s `pressure` field).

## Dependencies

REQUIRES `bb_core`, `bb_attrs`, `bb_filter`, `bb_json` (the public header's
`bb_sink_display_format_fn`/`_resolve_field` signatures use `bb_json_t`).
PRIV_REQUIRES `bb_cache`, `bb_cache_reactive`, `bb_display`, `bb_timer`,
`bb_log`. NEVER links LVGL.

## Platform support

| host | espidf | arduino |
|------|--------|---------|
| pure logic only (see below) | full | n/a |

The pure selection/formatting/table/config-validation logic
(`platform/host/bb_sink_display/`) is 100% host-testable, including every
`bb_sink_display_init()` rejection path (NULL args, CUSTOM-without-fn, the
multi-display seam gate, `evict_after_ms <= stale_after_ms`) via
`bb_sink_display_validate_config()`. The reactive wiring, timer tick, and
`bb_display` calls (`platform/espidf/bb_sink_display/bb_sink_display.c`) are
CI-smoke verified only -- they call into real
`bb_cache_reactive`/`bb_timer`/`bb_display`, which is device behavior, not
something this component re-mocks.

## See also

[bb_filter](../bb_filter/) / [bb_attrs](../bb_attrs/) — the selection
primitives this component consumes.
[bb_cache_reactive](../bb_cache_reactive/) — the on_change/on_register/
on_remove triad driving delivery.
[bb_display](../bb_display/) — the singleton render target for
DEFAULT_LINES.
`bb_collection` — the coming v2 field-catalog contribution vehicle (not yet
built; see the phasing note in `include/bb_sink_display.h`).

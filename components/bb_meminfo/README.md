# bb_meminfo

Canonical system-heap reader SSOT (KB #698/#699/#693) — the one component that
calls `heap_caps_*`.

## When to use / when not

Use `bb_meminfo_get()` whenever you need a heap/PSRAM/RTC/DRAM snapshot
(diagnostics, telemetry sources). Do not call `heap_caps_*`
directly from another component — route through `bb_meminfo` instead so
there is exactly one call site to audit. `bb_board`'s `bb_board_heap_*` /
`bb_board_psram_*` / `bb_board_rtc_*` / `bb_board_dram_static_bytes`
accessors already delegate here; existing callers of those functions need no
changes. This is a pure on-demand reader — it owns no routes, no `bb_pub`
source, and needs no `BB_INIT_REGISTER` hook.

## Public API

See [`include/bb_meminfo.h`](include/bb_meminfo.h). Key symbols:
`bb_meminfo_get(bb_meminfo_snapshot_t *out)` fills a snapshot covering the
`default`/`internal`/`dma`/`spiram` `heap_caps_*` regions plus RTC
used/total and internal-DRAM static bytes. ESP-IDF backs it with real
`heap_caps_*` reads; the host stub zeros every field.

`bb_meminfo_format(const bb_meminfo_snapshot_t *snap, char *buf, size_t len)`
formats a compact HEAP-ONLY diagnostic line from a snapshot (snprintf
semantics — buf is always NUL-terminated when len > 0): internal
free/min-ever-free/largest-free-block, spiram free, dma free, and
esp_min_free_heap. Pure formatting, identical on host and ESP-IDF; no
board/flash/app-size fields (that's a different domain — see `bb_board`).
Used by `examples/floor` and `examples/smoke` for a periodic serial heap
baseline line.

Public symbols in this component use the `bb_` prefix.

## bb_memreport — the unified memory report

`bb_memreport_get(bb_memreport_snapshot_t *out)` folds `bb_meminfo_get`'s
heap snapshot together with a per-region walk of every `bb_mem_arena_t`
registered via `bb_memreport_register_arena(name, arena)` — one snapshot
covering heap, bss (`out->heap.dram_static_bytes`), and each named arena's
free/used/peak-used bytes plus alloc/free/alloc-failed counters. Additive to
`bb_meminfo_get`/`_format` above; those stay heap-only by contract.

`bb_memreport_format(const bb_memreport_snapshot_t *snap, char *buf, size_t
len)` extends `bb_meminfo_format`'s heap-only line with `bss=<N>` and one
`<name>=free/peak/used` token per registered region (snprintf semantics,
truncation-safe).

`bb_memreport_deregister(name)` removes a previously registered arena
(no-op if never registered). Region capacity is `BB_MEMREPORT_MAX_REGIONS`
(Kconfig `CONFIG_BB_MEMREPORT_MAX_REGIONS`, default 8). PR1 scope is
arena-only — `bb_pool` consumers are a follow-up once `bb_pool` exposes its
backing arena handle.

`examples/floor` and `examples/smoke` use `bb_memreport_get`/`_format`
(instead of the heap-only `bb_meminfo_get`/`_format`) for their periodic
serial line; neither registers an arena, so their region_count is 0 and the
line is just heap + bss.

## Config knobs

| Kconfig | Default | Notes |
|---------|---------|-------|
| `CONFIG_BB_MEMREPORT_MAX_REGIONS` | 8 | `bb_memreport` region-registry capacity (`BB_MEMREPORT_MAX_REGIONS`). |

## Dependencies

<!-- BEGIN bbtool:deps -->
| Component | Kind | Role | Docs |
|-----------|------|------|------|
| `bb_core` | public | Foundational, near-zero-dep primitives every bb_* component builds on: the portable error type, the canonical clock, run-exactly-once, a contention-instrumented lock, byte-order helpers, memory accounting, and the reboot-reason codec. | [bb_core](../bb_core/README.md) |
| `bb_mem_arena` | public | — | [bb_mem_arena](../README.md) |
| `bb_registry` | private | — | [bb_registry](../README.md) |
<!-- END bbtool:deps -->

## Platform support

<!-- BEGIN bbtool:platform -->
| host | espidf | arduino |
|------|--------|---------|
| yes | yes | no |
<!-- END bbtool:platform -->

## Links

[![build](https://github.com/dangernoodle-io/breadboard/actions/workflows/build.yml/badge.svg)](https://github.com/dangernoodle-io/breadboard) [![coverage](https://coveralls.io/repos/github/dangernoodle-io/breadboard/badge.svg?branch=main)](https://github.com/dangernoodle-io/breadboard)

- Repository: [https://github.com/dangernoodle-io/breadboard](https://github.com/dangernoodle-io/breadboard)
- Wiki: [https://github.com/dangernoodle-io/breadboard/wiki](https://github.com/dangernoodle-io/breadboard/wiki)

## See also

`bb_board` (delegates its heap/psram/rtc/dram accessors here), `examples/floor`
(the floor's first telemetry source, read every tick and logged via `bb_log`).

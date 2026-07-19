# bb_log_event

"log" `bb_event` stream topic sink, carved out of `bb_log` (KB #708/#704).

## When to use / when not

Add `REQUIRES bb_log_event` when a firmware wants log lines surfaced as a
structured JSON `bb_data` key -- `bb_data_bind("log", &bb_log_event_wire_desc,
gather_fn)` in the composition root, then `bb_data_http_attach("log", "log")`
to serve it at `GET /api/events?topic=log` (the primary log transport; the
legacy `/api/logs` raw-text SSE route is retired). Composition-root ownership:
this component only forwards + gathers -- the `bb_data` bind and
`bb_data_http` attach are the consumer's call (see
`examples/floor/main/floor_app.c`'s `gather_log()`/`producers[]` and its
`bb_log_event_init()` call site, made once the HTTP server is up). Omit it
for headless/serial-only firmware that has no HTTP/`bb_data_http` stack --
`bb_log` itself has zero dependency back on this component, so composing
only `bb_log` never links this component or the web stack.

## Public API

Flat platform header (no `components/bb_log_event/include/` dir; see
`platform/espidf/bb_log_event/bb_log_event.h`), ESP-IDF only:
`bb_log_event_init(bb_http_handle_t server)` -- creates the forwarder
queue+task and hooks `bb_log`'s `s_log_vprintf` via
`bb_log_event_set_queue()`; `server` is accepted for API-shape parity with
the codegen call site but currently unused (no route registration --
`/api/events` is a `bb_data_http` composition-root concern). The portable
wire surface lives under `components/bb_log_event/include/bb_log_event_wire.h`:
`bb_log_event_wire_t` / `bb_log_event_wire_desc` (the `bb_serialize_desc_t`
SSOT for the "log" key) and `bb_log_event_gather()` (ESP-IDF only -- copies
the most recently forwarded payload into the caller's `bb_data_gather_fn`
scratch). Consumers also reach `bb_log_event_dropped()` (declared in
`bb_log.h`) for the forwarder's drop counter.

## Config knobs

`CONFIG_BB_LOG_EVENT_QUEUE_LEN` (default 48 on SPIRAM / 24 otherwise) --
forwarder queue depth between `bb_log`'s hot logging path and this
component's forwarder task.

## Dependencies

<!-- BEGIN bbtool:deps -->
| Component | Kind | Role | Docs |
|-----------|------|------|------|
| `bb_core` | public | Foundational, near-zero-dep primitives every bb_* component builds on: the portable error type, the canonical clock, run-exactly-once, a contention-instrumented lock, byte-order helpers, memory accounting, and the reboot-reason codec. | [bb_core](../bb_core/README.md) |
| `bb_data` | private | bb_data core binding table (B1-832) -- OWNS the `key -> (desc, gather)` binding table for the future bidirectional data path (the B1-828 epic replacing bb_pub + bb_sub + all bb_sink_*). | [bb_data](../bb_data/README.md) |
| `bb_http_server` | public | — | [bb_http_server](../README.md) |
| `bb_json` | public | — | [bb_json](../README.md) |
| `bb_log` | public | — | [bb_log](../README.md) |
| `bb_openapi` | public | — | [bb_openapi](../README.md) |
| `bb_serialize` | public | Format-neutral snapshot serialization: a descriptor SSOT + a pure walker + the bb_serialize_emit_t emit-vtable seam. | [bb_serialize](../bb_serialize/README.md) |
| `bb_str` | private | Portable string-safety helpers: strlcpy/field-fill semantics, key=value parsing, and hex<->bytes codec. | [bb_str](../bb_str/README.md) |
| `bb_task` | public | — | [bb_task](../README.md) |
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

`bb_log` (base component this was carved out of), `bb_log_http` (sibling
sink, GET/POST `/api/log/level` routes).

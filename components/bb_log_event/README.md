# bb_log_event

"log" `bb_event` stream topic sink, carved out of `bb_log` (KB #708/#704).

## When to use / when not

Add `REQUIRES bb_log_event` when a firmware wants log lines surfaced as a
structured JSON event stream at `GET /api/events?topic=log` (the primary log
transport; the legacy `/api/logs` raw-text SSE route is retired). Omit it
for headless/serial-only firmware (e.g. `examples/floor`) that has no
`bb_event`/`bb_http_server` stack — `bb_log` itself has zero dependency back
on this component, so composing only `bb_log` never links `bb_event` or the
web stack.

## Public API

No public header — this component is an ESP-IDF-only sink with no headers
under `include/`. It self-registers a REGULAR-tier `bb_init` entry
(`bb_log_event`, gated by `CONFIG_BB_LOG_EVENT_AUTO_ATTACH`) that attaches
the topic; consumers interact with it purely through `GET /api/events?topic=log`
and `bb_log_event_dropped()` (declared in `bb_log.h`).

## Config knobs

`CONFIG_BB_LOG_EVENT_AUTO_ATTACH` (default y, depends on
`BB_EVENT_ROUTES_AUTOREGISTER`) — auto-attach the "log" topic to
`/api/events`. `CONFIG_BB_LOG_EVENT_QUEUE_LEN` (default 48 on SPIRAM / 24
otherwise) — forwarder queue depth between `bb_log`'s hot logging path and
this component's forwarder task.

## Dependencies

<!-- BEGIN bbtool:deps -->
| Component | Kind | Role | Docs |
|-----------|------|------|------|
| `bb_core` | public | Foundational, near-zero-dep primitives every bb_* component builds on: the portable error type, the canonical clock, run-exactly-once, a contention-instrumented lock, byte-order helpers, memory accounting, and the reboot-reason codec. | [bb_core](../bb_core/README.md) |
| `bb_event` | public | — | [bb_event](../README.md) |
| `bb_event_routes` | public | — | [bb_event_routes](../README.md) |
| `bb_http_server` | public | — | [bb_http_server](../README.md) |
| `bb_json` | public | — | [bb_json](../README.md) |
| `bb_log` | public | — | [bb_log](../README.md) |
| `bb_openapi` | public | — | [bb_openapi](../README.md) |
| `bb_serialize` | public | Format-neutral snapshot serialization: a descriptor SSOT + a pure walker + the bb_serialize_emit_t emit-vtable seam. | [bb_serialize](../bb_serialize/README.md) |
| `bb_str` | private | — | [bb_str](../README.md) |
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

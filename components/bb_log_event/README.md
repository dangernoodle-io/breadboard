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
**REQUIRES:** `bb_core`, `bb_event`, `bb_event_routes`, `bb_init`, `bb_json`, `bb_log`, `bb_openapi`, `bb_task_registry`

**PRIV_REQUIRES:** `bb_http_server`
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
sink, GET/POST `/api/log/level` routes), `bb_sink_ws` (subscribes to the
"log" topic when both are composed).

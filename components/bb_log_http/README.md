# bb_log_http

GET/POST `/api/log/level` routes sink, carved out of `bb_log` (KB #708/#704).

## When to use / when not

Add `REQUIRES bb_log_http` when a firmware wants runtime log-level control
over HTTP (`GET`/`POST /api/log/level`). Omit it for headless/serial-only
firmware (e.g. `examples/floor`) that has no `bb_http_server` stack —
`bb_log` itself has zero dependency back on this component, so composing
only `bb_log` never links the web stack.

## Public API

No public header — this component is an ESP-IDF-only route sink with no
headers under `include/`. It self-registers PRE_HTTP (route-count reserve)
and REGULAR-tier (`bb_log_register_routes`, gated by `CONFIG_BB_LOG_ROUTES`)
`bb_init` entries that register the routes against `bb_log`'s portable
level-registry API (`bb_log.h`).

## Config knobs

`CONFIG_BB_LOG_ROUTES` (default y) — build and register the
GET/POST `/api/log/level` routes. `CONFIG_BB_LOG_SSE_KEEPALIVE_MS`
(default 5000) — vestigial, kept only to avoid build breakage on boards
still referencing it via sdkconfig; unused.

## Dependencies

<!-- BEGIN bbtool:deps -->
| Component | Kind | Role | Docs |
|-----------|------|------|------|
| `bb_http_server` | private | — | [bb_http_server](../README.md) |
| `bb_init` | private | — | [bb_init](../README.md) |
| `bb_log` | private | — | [bb_log](../README.md) |
<!-- END bbtool:deps -->

## Platform support

<!-- BEGIN bbtool:platform -->
| host | espidf | arduino |
|------|--------|---------|
| no | yes | no |
<!-- END bbtool:platform -->

## Links

[![build](https://github.com/dangernoodle-io/breadboard/actions/workflows/build.yml/badge.svg)](https://github.com/dangernoodle-io/breadboard) [![coverage](https://coveralls.io/repos/github/dangernoodle-io/breadboard/badge.svg?branch=main)](https://github.com/dangernoodle-io/breadboard)

- Repository: [https://github.com/dangernoodle-io/breadboard](https://github.com/dangernoodle-io/breadboard)
- Wiki: [https://github.com/dangernoodle-io/breadboard/wiki](https://github.com/dangernoodle-io/breadboard/wiki)

## See also

`bb_log` (base component this was carved out of), `bb_log_event` (sibling
sink, structured "log" `bb_event` stream at `/api/events?topic=log`).

# bb_wifi_http

Opt-in STA route bundle for `bb_wifi` (PR1 of the bb_wifi split, KB 781/809).
Registers `GET /api/wifi`, `POST /api/scan`, and — when
`CONFIG_BB_WIFI_RECONFIGURE=y` (default) — `PATCH /api/wifi` on the shared
`bb_http_server`. This is a route **bundle**, not a server: there is one
`bb_http_server` per device; `bb_wifi_http` composes on top of it and on top
of `bb_wifi` (the STA core).

## Why a separate component

`bb_wifi` (the STA core: connect, reconnect FSM, recovery, credentials) does
not need HTTP, JSON, or OpenAPI to run — those are needed only to *expose*
STA state over `/api/wifi`. Per the heap-first opt-in principle (KB 809),
the STA core sheds `bb_http`/`bb_json`/`bb_openapi` and a minimal STA-only
image can omit `bb_wifi_http` entirely, paying none of that dependency's
heap.

## What moved here (relocation, no behavior change)

- `GET /api/wifi`, `POST /api/scan`, and the `CONFIG_BB_WIFI_RECONFIGURE`
  `PATCH /api/wifi` handler — unchanged from `bb_wifi`'s prior
  `platform/espidf/bb_wifi/bb_wifi_routes.c`.
- `bb_wifi_emit_section` / `bb_wifi_emit_status` — the `bb_json_t`-based wifi
  section emitters (SSOT for the `/api/wifi` and `/api/health` "network"
  wire format). Declared in `bb_wifi_http.h`; `bb_wifi.h` no longer includes
  `bb_json.h`.
- The `CONFIG_BB_WIFI_ROUTES_AUTOREGISTER` Kconfig knob (same symbol name,
  relocated ownership — existing sdkconfig files need no changes).

## Composition

Add `bb_wifi_http` to your component's `REQUIRES`/`SMOKE_REQUIRES` alongside
`bb_wifi` and `bb_http_server` to keep the `/api/wifi` routes. Consumers
that only need the STA connection (no HTTP surface) can omit it.

Other components that need the JSON emitters without the HTTP routes (e.g.
`bb_health`'s `/api/health` "network" section, which already depends on
`bb_http`/`bb_json`/`bb_openapi` itself) depend on `bb_wifi_http` for
`bb_wifi_emit_status` rather than reaching into `bb_wifi`.

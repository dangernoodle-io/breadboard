# bb_wifi_http

Opt-in STA route bundle for `bb_wifi` (PR1 of the bb_wifi split, KB 781/809).
Registers `GET /api/wifi`, `GET /api/diag/wifi`, `POST /api/wifi/scan`, and —
when `CONFIG_BB_WIFI_RECONFIGURE=y` (default) — `PATCH /api/wifi` on the
shared `bb_http_server`. This is a route **bundle**, not a server: there is one
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

- `GET /api/wifi`, `POST /api/wifi/scan`, and the `CONFIG_BB_WIFI_RECONFIGURE`
  `PATCH /api/wifi` handler — unchanged from `bb_wifi`'s prior
  `platform/espidf/bb_wifi/bb_wifi_routes.c`.
- `bb_wifi_emit_section` / `bb_wifi_emit_status` — the `bb_json_t`-based wifi
  section emitters (formerly SSOT for the `/api/wifi` and `/api/health`
  "network" wire format). Both since migrated off `bb_json` to
  `bb_serialize_desc_t` (`bb_wifi_emit_section` -> B1-1057's
  `bb_wifi_http_info_wire_desc`; `bb_wifi_emit_status` deleted outright,
  B1-1149 -- its only caller was a host test, `/api/health`'s "network"
  section renders through `bb_health_wire_desc` instead). `bb_wifi.h` no
  longer includes `bb_json.h`; neither does `bb_wifi_http.h`.
- The `CONFIG_BB_WIFI_ROUTES_AUTOREGISTER` Kconfig knob (same symbol name,
  relocated ownership — existing sdkconfig files need no changes).
- `GET /api/diag/wifi` (B1-969) — rehomed from the now-dissolved
  `bb_net_health`, reduced to `bb_wifi`-native fields only.

## Composition

Add `bb_wifi_http` to your component's `REQUIRES`/`SMOKE_REQUIRES` alongside
`bb_wifi` and `bb_http_server` to keep the `/api/wifi` routes. Consumers
that only need the STA connection (no HTTP surface) can omit it.

`bb_health`'s `/api/health` "network" section still depends on `bb_wifi_http`
for `bb_wifi_http_format_bssid` (the shared colon-hex formatter), but not for
a JSON emitter: it gathers `bb_wifi_get_info`'s fields directly and renders
through its own `bb_health_wire_desc` (`components/bb_health/
bb_health_wire_priv.h`), a `bb_serialize_desc_t`.

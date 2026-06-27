# ESP-IDF Bootstrap Component Library

Standalone ESP-IDF bootstrap component library. Reusable wifi provisioning, NVS, HTTP server, OTA, log streaming, display, and board abstraction components for any ESP-IDF firmware project.

## Public symbol prefix

All public C symbols use prefix `bb_`.

## Portability discipline

Public headers must not include `esp_*.h` or `freertos/*.h` outside `#ifdef ESP_PLATFORM`. This is enforced so non-ESP-IDF platform backends (e.g. Arduino) can coexist without breaking consumers.

The `bb_info` component demonstrates the portable-header pattern: its public extender callback takes a `bb_json_t` handle (from `bb_json`) rather than a backend-specific pointer, so consumer headers stay free of cJSON/ArduinoJson includes. Platform-specific logic lives under `platform/espidf/` and `platform/host/`.

For the Arduino backend specifically:
- Log format strings (`bb_log_*` calls) should be wrapped in `F()` to force them into PROGMEM to preserve SRAM. Deferred until a real project hits SRAM pressure.
- Backend implementations are in `platform/arduino/<component>/` as `.cpp` files (not `.c`).
- `bb_http` on Arduino batches the entire response into a single `client.write()` call. Request bodies are not supported.
- `bb_led_anim` and `bb_button_events` with `auto_start_timer=true` return `BB_ERR_UNSUPPORTED` on Arduino; consumers must call tick manually.

## API conventions

Public headers under `components/*/include/` follow these rules so that the same header compiles unchanged on ESP-IDF, Arduino, and host test backends.

- **Return `bb_err_t`**, never `esp_err_t` (or any other platform type). `bb_err_t` is a typedef for `esp_err_t` under ESP-IDF and `int` elsewhere — values pass through unchanged.
- **Cross-cutting types live in `bb_core`.** `bb_err_t`, `BB_OK`/`BB_ERR_*` macros, `bb_http_handle_t`, `bb_http_request_t`, `bb_http_pause_cb_t`, and `bb_http_resume_cb_t` are defined in `components/bb_core/include/bb_core.h`. REQUIRES `bb_core` for these types — never `bb_nv` for the error type or `bb_http` for the handle typedef alone. `bb_ota_pause/resume_cb_t` and `bb_update_check_pause/resume_cb_t` are aliases for the `bb_core.h` typedefs. Portable `bb_clock_now_ms()` lives in `bb_core/include/bb_clock.h`.
- **Log via `bb_log_*`**, never `ESP_LOG*`. Portable impls live under `platform/<backend>/bb_log/`.
- **No `#ifdef ESP_PLATFORM` in public headers.** If a function needs an ESP-IDF type in its signature, wrap it behind an opaque `bb_*` handle (see `bb_http_request_t` wrapping `httpd_req_t *`).
- **No ESP-IDF headers** (`esp_err.h`, `esp_http_server.h`, `nvs_flash.h`, …) included transitively from public headers. Private `.c` files may include them freely.
- **Streaming JSON arrays:** For handlers emitting JSON arrays of N small objects, prefer `bb_http_resp_json_arr_*` (begin/emit/end) over building one large `bb_json_t` tree. On ESP-IDF, items stream in true chunked chunks so only one per-item subtree lives in memory; on Arduino/host it buffers for compatibility. Same external output; one API for both.

When extending an existing component: match the existing style even if older code predates these rules; a PR that flips 10 functions without migrating the rest is churn.

## Avoiding audit-class regressions

These rules encode the recurring defect classes found in the v0.62.0→main audit; follow them so they don't recur.

- **Kconfig knobs must bridge `CONFIG_`.** Every `CONFIG_BB_*` symbol must be consumed via the bridge pattern — `#ifdef ESP_PLATFORM` → `#include "sdkconfig.h"` → `#ifdef CONFIG_BB_X` `#define BB_X CONFIG_BB_X` `#endif`, then `#ifndef BB_X` `#define BB_X <default>` `#endif`. **Never** write a bare `#ifndef BB_X #define BB_X <lit>` alongside a `CONFIG_BB_X` Kconfig: the `#ifndef` shadows the generated symbol and the knob is silently inert (this shipped 3× — `bb_net_health`, `bb_power_health`, `bb_pub`). Reference: `bb_clock.h`, `bb_net_health.h`. C default and Kconfig default must match.
- **Reuse-or-extract shared helpers — don't re-hand-roll an idiom.** Cross-cutting primitives live in `bb_core` (e.g. `bb_clock`, `bb_byte_order`, `bb_mem`); component-scoped helpers live in that component's `include/` header. Before hand-rolling a common shape (SPIRAM-preferred alloc, lock+capture, status/decode tables, recv-body, schema-assemble, internal-heap queries…), find the existing helper — `find_similar_code`/`semantic_code_search` (code-graph-mcp) or `grep components/*/include bb_core`. If a shape appears in ≥2 places, extract it (portable header + `platform/{espidf,host}` impls) rather than copy. The inventory is intentionally not listed here — discover it with the tools above so this rule never goes stale.
- **Branch coverage on new conditional code.** A refactor adding a `switch`/decode/guard must have tests exercising **every** branch — Coveralls is a required check and blocks on an uncovered new branch. For error branches unreachable with the real allocator (malloc-fail), inject via a `*_set_malloc` hook guarded by `BB_<NAME>_TESTING` + `test/test_host/test_alloc_inject.h`.
- **Timestamps go through the canonical bb_clock helper.** Exposed absolute time points use `bb_clock_now_ms64()` (u64 ms, no 49.7-day wrap); durations/ages use `_age_ms`/`_age_s` computed at read time; epoch wall-clock uses `_epoch_s`. NEVER hand-roll `bb_timer_now_us()/1000` or raw `esp_timer_get_time()` for an exposed timestamp, and never feed a u32 `bb_clock_now_ms()` into a u64 `*_ms` field. Internal duration arithmetic (e.g. `last_publish_ms`, `last_sample_ms` in `bb_pub`) may keep u32 — wrapping subtraction is intentional there.

**Periodic idiom-duplication sweep.** A diff-scoped review misses cross-file duplication (it's how the SPIRAM/status/query/section/HAL dup shipped). Before cutting a release, run the whole-repo idiom sweep (workflow `idiom-sweep` / `.claude/workflows/idiom-sweep.md`): parallel finders over allocation, concurrency, http-serialization, config/peripheral, and clock/timestamp idioms, each reporting ≥2-site repeats with a proposed helper. The header/type-leak CI lint (B1-263) is the complementary enforcement for REQUIRES/portability hygiene.

## Logging

Use `bb_log_{e,w,i,d,v}(tag, fmt, ...)` macros for all breadboard component code. On ESP-IDF these expand to `ESP_LOG{E,W,I,D,V}`; on host they map to `fprintf` (debug/verbose compile out to keep test output clean).

The `bb_log_stream` ringbuffer is lazy-allocated on the first call to `bb_log_stream_init()` (heap-allocated, SPIRAM-preferred with fallback to default heap), reducing BSS footprint by ~6 KB when no log consumer is attached. The allocation is idempotent; subsequent calls return `BB_OK` immediately. Buffer size is tunable via `CONFIG_BB_LOG_STREAM_BUF_BYTES` (default 6144 bytes).

`bb_log.h` does **not** transitively expose `bb_nv.h` or `freertos/FreeRTOS.h`; components that previously relied on this transitive inclusion must add explicit includes.

## Layout

- Components under `components/<name>/` with public headers.
- Platform-specific implementations under `platform/espidf/` and `platform/arduino/`.
- Component names are prefixed `bb_`.

## Header visibility and component coupling

These rules govern which headers go where and how components depend on each other.

### Public vs private headers

- `components/<name>/include/` is the **public** surface. Only headers intended for cross-component use belong here.
- `_port.h` / `_internal.h` / `_priv.h` headers are **private** — put them next to the implementation, not in `include/`.
- Platform impls (`platform/<backend>/<name>/`) include private headers via relative paths; consumers cannot reach them.

### REQUIRES vs PRIV_REQUIRES

- **Decisive test:** a dep goes in `REQUIRES` ONLY if THIS component's public header (`include/`) includes that dep's header or uses its types. Everything else → `PRIV_REQUIRES`. **Default to `PRIV_REQUIRES`.**
- `REQUIRES` transitively exposes the dep's entire surface to all consumers — it is not a "strong recommendation" or "encouraged dependency".
- Test-only deps go in the `test_*` env's `lib_deps`, not the component CMakeLists.

### Component dependency hygiene

**No ESP-IDF / third-party types in public headers.** Wrap behind an opaque `bb_*` handle:
- `bb_http_request_t` (`void*`) hides `httpd_req_t*`
- `bb_json_t` (`void*`) hides `cJSON*`

If a new type would force an `#include <esp_…>` or `#include "cJSON.h"` into a public header, define a `bb_*` opaque alias instead.

**Exception — large-panel LVGL backends.** `ek79007` and future big-panel backends deliberately expose `lv_obj_t*` in their public header and keep `esp_lvgl_port` in `REQUIRES`. This is the documented escape hatch for panels where hand-blitting is impractical. See the Display section. Do NOT apply this exception to small panels or non-display components.

**High-risk dep watchlist** — these MUST be `PRIV_REQUIRES` unless a public header genuinely needs them:
- `esp_driver_*`, `esp_lcd`, `esp_http_server`, `esp_timer`, `esp_system`
- `app_update`, `espressif__mdns`
- `json` / `cJSON`

**Self-check before committing a new component:**
```sh
grep -nE '#include <esp_|#include "esp_|cJSON|i2c_master|httpd_' components/<name>/include/*.h
```
This should be empty (outside the LVGL exception). The CI lint step (`make check`) enforces this automatically via `scripts/check_lint.sh` (B1-263).

### Tests must not reach into other components

- Tests may `#include` any public header (`components/<name>/include/<name>.h`).
- Tests MUST NOT `#include` private implementation headers. If a test needs a hook, the component exposes it via `<name>_test.h` guarded by `BB_<NAME>_TESTING`.

### Includes at file top

`#include` lives at the top of the file. Mid-file `#include` directives hide dependencies; conditional gating belongs around the *use* of the symbol, not the include itself.

## Embedding assets

Use the CMake helper `bb_embed_assets()` in `cmake/bb_embed.cmake` to embed binary assets (HTML, fonts, images, etc.) into firmware. The helper wraps the raw `scripts/embed_html.py` CLI tool, which converts a file to a gzipped C byte-array source file: `python3 scripts/embed_html.py <input> <output.c> <symbol>`. Components include the helper with `include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_embed.cmake")` and call it before `idf_component_register` to populate `SRCS`. The helper avoids duplicating `add_custom_command` boilerplate across breadboard components and downstream consumers (TaipanMiner, snugfeather).

## Registry (handler-lifecycle)

`bb_registry` holds three ordered lists — EARLY → PRE_HTTP → REGULAR — driven by two app calls: `bb_registry_init_early()` then `bb_registry_init()`. `bb_registry_init()` walks PRE_HTTP, optionally autostarts the HTTP server (`CONFIG_BB_HTTP_AUTOSTART`, default y), then walks the regular tier. Components self-register at link time via a macro; disabling the corresponding `CONFIG_BB_<NAME>_AUTOREGISTER` Kconfig (default y) removes the registration without touching the public API.

**EARLY tier** — NVS, WiFi init, board bring-up (no server arg):
```c
BB_REGISTRY_REGISTER_EARLY(bb_<comp>, bb_<comp>_init);  // bb_err_t fn(void)
```

**PRE_HTTP tier** — tasks that must run after EARLY but before the HTTP server (no server arg):
```c
BB_REGISTRY_REGISTER_PRE_HTTP(bb_<comp>, bb_<comp>_init);  // bb_err_t fn(void)
```

**Regular tier** — HTTP route registration (server arg):
```c
BB_REGISTRY_REGISTER(bb_<comp>, bb_<comp>_init);        // order=0 (default)
BB_REGISTRY_REGISTER_N(bb_<comp>, bb_<comp>_init, N);   // explicit order N
```
`BB_REGISTRY_REGISTER_N` accepts an integer `order` as the third argument. The regular-tier walker sorts entries by `.order` ascending before invoking inits — lower numbers run first. Ties preserve registration (insertion) order, i.e. the sort is stable. `BB_REGISTRY_REGISTER` is shorthand for `BB_REGISTRY_REGISTER_N(..., 0)`. **N is a sort key only — not a route-count hint.** Example: `bb_event_routes` registers at order 0; `bb_update_check` registers at order 4 so `bb_event_routes_init` (which sets `s_cfg.initialized = true`) has already run before `bb_update_check` calls `bb_event_routes_attach`.

All three tiers require the matching CMake helper (in `cmake/bb_registry.cmake`) to prevent garbage-collection under PlatformIO:

```cmake
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
bb_registry_force_register(${COMPONENT_LIB} bb_<name>)           # regular
bb_registry_force_register_early(${COMPONENT_LIB} bb_<name>)     # early
bb_registry_force_register_pre_http(${COMPONENT_LIB} bb_<name>)  # pre_http
```

Today this pattern owns — regular: `bb_ota_pull`, `bb_ota_push`, `bb_info`, `bb_log` (routes), `bb_manifest`, `bb_ota_validator`, `bb_wifi` (routes), `bb_system` (routes), `bb_openapi`, `bb_mdns`, `bb_event_routes`, `bb_update_check`, `bb_diag` (routes). Early: `bb_log_stream`, `bb_nv_flash`, `bb_nv_config`, `bb_wifi` (STA init via `CONFIG_BB_WIFI_AUTOREGISTER`), `bb_diag_panic`, `bb_event`. PRE_HTTP: `bb_http_reserve_routes(N)` is vestigial (see Route-sizing model below) but companion init functions may still call it harmlessly. HTTP server autostart is gated on `CONFIG_BB_HTTP_AUTOSTART` (default y); disable it if CORS or OpenAPI config must precede server start. Socket reservation for non-httpd usage (stratum TCP, mDNS UDP, transient outbound) is tunable via `CONFIG_BB_HTTP_LWIP_RESERVE` (default 3, range 1–6) — lower it to give httpd more headroom, raise it to preserve slots for outbound work.

**Event loop noise suppression (B1-306).** `bb_http` registers a no-op handler for `ESP_HTTP_SERVER_EVENT` on the default event loop at server start to suppress esp_event "no handlers" DEBUG spam that self-amplifies when the `/api/logs` SSE viewer is open.

**Route-sizing model.** All `/api/*` routes are served by per-method wildcard httpd handlers (`GET/POST/PUT/PATCH/DELETE /api/*`, registered at server start before any consumer asset `GET /*`) that dispatch internally via the `bb_api_dispatch` table (`BB_HTTP_API_DISPATCH_CAP`, default 64). A high-watermark `bb_log_w` fires once when count reaches `CAP-8`; overflow returns `BB_ERR_NO_SPACE` (non-fatal, logged). `/api/*` routes do **not** consume httpd handler slots. `max_uri_handlers` (`BB_HTTP_MAX_URI_HANDLERS`, default 12, constant) covers only the fixed wildcards (5 api + OPTIONS + GET asset) plus headroom for non-`/api` routes (`/save`, captive `/*`). `bb_http_reserve_routes()` is vestigial (ABI kept; body is a no-op on espidf; calls from existing PRE_HTTP companions are harmless). When adding a new `/api/` endpoint, no PRE_HTTP companion or reserve call is needed — just call `bb_http_register_route` in the regular tier. When adding a non-`/api` httpd route (e.g. a captive-portal path), raise `BB_HTTP_MAX_URI_HANDLERS` in your `sdkconfig` if headroom is exhausted.

`bb_http_register_route` and related registration functions return `BB_ERR_NO_SPACE` when the route registry is full. Registry capacity is tunable via `CONFIG_BB_HTTP_ROUTE_REGISTRY_CAP` (default 64). After the regular-tier walk, `bb_registry_init()` audits the registry count and panics if overflow occurred (`CONFIG_BB_HTTP_ROUTE_REGISTRY_STRICT`, default y) or emits a high-watermark warning at count >= CAP-8.

Duplicate (method, path) registrations in the `/api/*` dispatch table are detected at `bb_api_dispatch_add` time: the second registration is warned (`bb_log_w`) and dropped (first-wins); callers receive `BB_ERR_INVALID_STATE`. Enable `CONFIG_BB_HTTP_ROUTE_DUP_STRICT` (default n) to escalate a duplicate to a hard `assert` — useful in CI/smoke builds to catch accidental route collisions early.

**Display self-registration.** `BB_DISPLAY_AUTOREGISTER(chip, kconfig_sym, backend_ptr)` macro (in `bb_display_spi_common`) replaces per-driver constructor boilerplate. SSD1306 framebuffer is heap-allocated at init; `bb_display_ssd1306_init` can return `BB_ERR_NO_MEM`. ili9341 and st77xx share a bounce buffer via `bb_display_spi_common` for blit and clear operations.

The `bb_nv_config` component persists device configuration to NVS: WiFi credentials, hostname (32 chars max, RFC1123 charset), and DHCP/mDNS feature flags. Hostname is available via `bb_nv_config_hostname()` and settable via `bb_nv_config_set_hostname()` with validation for leading/trailing hyphens and non-alphanumeric characters. RTC slow-memory creds backup (`CONFIG_BB_NV_CREDS_RTC_BACKUP`, default y): mirrors ssid/pass/provisioned into `RTC_NOINIT_ATTR` so a crash-induced NVS erase doesn't strand a headless board; restored and healed on next boot (`bb_nv_config_creds_restored()`); erase observability via `bb_nv_config_was_erased()` (both always defined under ESP_PLATFORM).

`bb_wifi_autoinit` (EARLY tier) reads the hostname from `bb_nv_config_hostname()` and applies it before connecting so DHCP/mDNS see the right name on first packet; on validated builds it retries the STA connect indefinitely (30 s backoff, `CONFIG_BB_WIFI_RETRY_FOREVER_WHEN_VALIDATED`, default y) so a network outage doesn't knock the device into AP mode; errors are swallowed so the EARLY walker continues for provisioning flows.

`bb_wifi` routes (regular tier, `platform/espidf/bb_wifi/bb_wifi_routes.c`): `GET /api/wifi` (connection info), `POST /api/scan` (scan trigger), and — when `CONFIG_BB_WIFI_RECONFIGURE=y` (default) — `PATCH /api/wifi {ssid, password}` → 202 + deferred reboot via `bb_wifi_reconfigure` (brick-safe; reserve bumps from 2 to 3).

The `wifi_reconn` FSM (`platform/espidf/bb_wifi/wifi_reconn.c`) drives reconnect. ST_CONNECTING is bounded by `WIFI_RECONN_CONNECTING_TIMEOUT_MS` (30 s): if neither GOT_IP nor DISCONNECT arrives, the task calls `wifi_reconn_policy_on_connect_timeout`, re-issues `esp_wifi_disconnect` + `esp_wifi_connect`, and stays in ST_CONNECTING, escalating to the safeguard reboot (`do_safeguard_reboot`) after the 5-min persistent-fail window — same path used by disconnect-triggered reboots.

**Lost-IP recovery (B1-365).** When `IP_EVENT_STA_LOST_IP` fires while the reconnect manager is active, `bb_wifi.c` calls `wifi_reconn_on_lost_ip()` which bumps `s_state.lost_ip_count`, `last_lost_ip_us`, and `reason_histogram[WIFI_REASON_BB_LOST_IP]` (sentinel 99, reserved breadboard value — esp_wifi reasons use 1-24/53-67/200-208; 99 is free and < 256) via `wifi_reconn_policy_on_lost_ip`, then issues `esp_wifi_disconnect()` (without setting `s_self_disconnect`) so the resulting `WIFI_EVENT_STA_DISCONNECTED` flows into `wifi_reconn_on_disconnect()` and drives full recovery. A second trigger: `reconn_task` in `ST_IDLE` polls every `CONFIG_BB_WIFI_NO_IP_WATCHDOG_S` seconds (default 60 s, bridged as `WIFI_RECONN_NO_IP_WATCHDOG_MS`) via `esp_wifi_sta_get_ap_info` + `bb_wifi_has_ip()` — boot-safe since `esp_wifi_sta_get_ap_info` returns non-`ESP_OK` when not associated. Recovery is observable via `bb_wifi_get_lost_ip_count()` / `bb_wifi_get_lost_ip_age_s()` (public API) and as `lost_ip_recoveries` / `lost_ip_age_s` in the `net.health` SSE topic and `/api/health` net section.

**Inactive-time zombie recovery (B1-369).** `CONFIG_BB_WIFI_NO_IP_WATCHDOG_ENABLE` (default n) gates both mode-a and mode-b recovery. When disabled, `ST_IDLE` uses `portMAX_DELAY` (no periodic watchdog). When enabled: (mode-a) ST_IDLE watchdog fires every `CONFIG_BB_WIFI_NO_IP_WATCHDOG_S` seconds; (mode-b) `esp_wifi_set_inactive_time(WIFI_IF_STA, CONFIG_BB_WIFI_INACTIVE_TIME_S)` (default 45 s, minimum 3) is called after `esp_wifi_start()` so the driver emits `WIFI_EVENT_STA_DISCONNECTED` after beacon starvation, flowing into the normal reconnect FSM path — the safeguard-reboot chain arms automatically via the existing `wifi_reconn_on_disconnect` → `wifi_reconn_policy_on_disconnect` → `first_fail_us` → 5-min window path. `bb_wifi_restart_sta()` performs a full stop/start cycle instead of a bare `esp_wifi_disconnect()`: it sets `s_sta_restarting` (suppresses the auto-connect fired by `WIFI_EVENT_STA_START`), calls `wifi_reconn_absorb_next_disconnect()` (absorbs the synthetic `WIFI_EVENT_STA_DISCONNECTED` from `esp_wifi_stop()`), restarts the driver, re-applies the saved `s_sta_config` and inactive-time, then calls `esp_wifi_connect()` explicitly. Both FSM reconnect-action sites (immediate retry + backoff elapsed) and `bb_wifi_force_reassociate()` call `bb_wifi_restart_sta()` when enabled; fall back to `esp_wifi_connect()` / `esp_wifi_disconnect()` when disabled.

**Egress-dead probe (B1-371).** Additive to B1-369. When `BB_WIFI_NO_IP_WATCHDOG_ENABLE` is on, `ST_IDLE` ticks every `CONFIG_BB_WIFI_EGRESS_PROBE_S` (default 30 s, bridged as `WIFI_RECONN_EGRESS_PROBE_MS`) rather than the 60 s no-IP cadence (the no-IP check still runs every tick — it is cheap). On each tick where `associated && has_ip`, the task calls `bb_wifi_gateway_reachable(1500)` — a lazy-session `esp_ping` (one ICMP echo, 1.5 s timeout) to the current STA default gateway. Consecutive failures are tracked by `wifi_reconn_policy_on_egress_probe` (`wifi_reconn_policy.{c,h}`) in `s_state.egress_fail_streak`; at `CONFIG_BB_WIFI_EGRESS_PROBE_FAILS` (default 3) it bumps `egress_dead_count`, arms `first_fail_us`, increments `reason_histogram[WIFI_REASON_BB_EGRESS_DEAD]` (sentinel 100, < 256, reserved), resets the streak, and returns `WIFI_RECONN_ACTION_RECONNECT_NOW`. The watchdog then calls `bb_wifi_restart_sta()` to recover. This detects b2-class egress zombies (board has IP + L2 association but no real egress to DNS/MQTT, e.g. AP keeps beaconing but upstream gateway is stuck) where `inactive_time` never fires (beacons arrive normally). **Public API**: `bb_wifi_ping(esp_ip4_addr_t, timeout_ms, *out_reachable)` — esp_ping session created lazily and reused (one binary semaphore for signaling; mutex guards session create/reuse; ~3–4 KB resident once initialized; ALWAYS compiled so CI validates the symbol); `bb_wifi_gateway_reachable(timeout_ms)` — gets gateway via `esp_netif_get_ip_info(s_sta_netif, &ip_info)`, pings `ip_info.gw`. Both declared inside `#ifdef ESP_PLATFORM` in `bb_wifi.h` (uses `esp_ip4_addr_t` from `esp_netif_ip_addr.h`). Host stub: `bb_wifi_get_egress_dead_count()` returns 0; `bb_wifi_host_set_gateway_reachable(bool)` test hook (gated by `BB_WIFI_TESTING`). Telemetry: `egress_dead_recoveries` added to `bb_net_health_status_t` + schema + emit + publish_snapshot + get_status (mirrors `lost_ip_recoveries` pattern). Kconfig: `CONFIG_BB_WIFI_EGRESS_PROBE_S` (int, default 30, range 5–600), `CONFIG_BB_WIFI_EGRESS_PROBE_FAILS` (int, default 3, range 1–20), both `depends on BB_WIFI_NO_IP_WATCHDOG_ENABLE`. Inactive-time kept additive: b1 boards still benefit from beacon-loss disconnect; b2 boards get the egress probe as a second layer.

## Satellite / extender opt-in convention

A **satellite** is a small component that contributes a section to an existing endpoint (e.g. a named section on `/api/health` via `bb_health_register_section`, or on `/api/info` via `bb_info_register_section`) without owning the route itself. Three tiers exist:

**Tier 1 — In-component Kconfig gate** (`CONFIG_BB_<X>_ROUTES_AUTOREGISTER`, default n): the extender exposes the owning component's own data and lives inside that component. No separate component. Examples: `bb_power_routes`, `bb_fan_routes`, `bb_log_routes`.

**Tier 2 — Separate component + Kconfig auto-register** (`CONFIG_BB_<X>_AUTOREGISTER`, default n for health/info satellites; default y for infrastructure routes): the satellite lives in its own component directory, bridges two independent dep closures, and self-registers via `BB_REGISTRY_REGISTER[_N]` in the ESP-IDF `.c` file, gated by `#if CONFIG_BB_<X>_AUTOREGISTER`. The CMakeLists conditionally calls `bb_registry_force_register` under `if(CONFIG_BB_<X>_AUTOREGISTER)`. Examples after B1-347: `bb_temp`, `bb_mqtt_info`, `bb_led_info`. Pre-existing examples: `bb_net_health`, `bb_pub_*`.

**Tier 3 — Manual register() — DEPRECATED for new satellites.** The three legacy satellites (`bb_temp`, `bb_mqtt_info`, `bb_led_info`) keep their manual fns (`bb_temp_register_info`, `bb_mqtt_register_health`, `bb_led_register_info`) as escape hatches for consumers with ordering constraints. **Use auto XOR manual** — enabling `AUTOREGISTER` while also calling the manual fn double-registers the section (second registration is warned and dropped; first-wins per route registry policy). Flipping these to `default y` and removing the manual calls from TaipanMiner is a cross-repo follow-up, not part of this change.

**Naming split:** `CONFIG_BB_<NAME>_AUTOREGISTER` for core/infrastructure components and health/info satellites (this file); `CONFIG_BB_<NAME>_AUTO_ATTACH` for event-topic satellites that attach a `bb_event` topic to `bb_event_routes` (see Auto-attach convention below). `bb_display_info` uses `CONFIG_BB_DISPLAY_INFO_AUTO_ATTACH` — the one legacy exception to the `_AUTOREGISTER` name on a satellite; do **not** rename it.

## bb_alert — one-shot alert event channel

`bb_alert` is an opt-in component for emitting discrete, one-shot alert events on the `"alert"` `bb_event` topic (non-retained, non-replay). It is designed for transient failure or state-change signals; durable state belongs in retained `bb_cache` topics.

**Opt-in:** `CONFIG_BB_ALERT_ENABLE` (default `n`). When disabled, `bb_alert_emit()` compiles to a safe `static inline` no-op at zero cost. Consumers can call `bb_alert_emit()` unconditionally without `#ifdef` guards.

**Component location:** `components/bb_alert/` (header + Kconfig + CMakeLists). Shared implementation: `platform/host/bb_alert/bb_alert.c` (compiled host + espidf). ESP-IDF registry glue: `platform/espidf/bb_alert/bb_alert_espidf.c`.

**Topic:** `"alert"` — non-retained. Registered and attached at regular-tier order 4 (after `bb_event_routes` at order 0) when `CONFIG_BB_ALERT_AUTOREGISTER=y` (depends on `BB_ALERT_ENABLE && BB_EVENT_ROUTES_AUTOREGISTER`).

**API:**
- `bb_alert_emit(type, sev, fill, ctx)` — emit a one-shot alert. `type` is a string label (e.g. `"wifi_lost_ip"`). `sev` is `BB_ALERT_INFO` / `BB_ALERT_WARNING` / `BB_ALERT_CRITICAL`. `fill` is an optional `bb_alert_fill_fn` callback that adds extra fields to the JSON object. `ctx` is passed through to `fill`.
- `bb_alert_register()` — registers the `"alert"` topic; called automatically by the registry init or manually in test teardown.

**Severity threshold:** `BB_ALERT_MIN_SEVERITY` (Kconfig default 0 = INFO, C default matches). Alerts below the threshold are silently dropped before any JSON allocation. In test builds, override with `bb_alert_set_min_severity_for_test(sev)`.

**Envelope:** `{type: string, severity: int, uptime_ms: int, ...fill fields}`. The JSON is serialized with `bb_json` and posted via `bb_event_post`.

**Consumers:**
- `wifi_lost_ip` (BB_ALERT_WARNING) in `platform/espidf/bb_wifi/wifi_reconn.c`: fired in `wifi_reconn_on_lost_ip()` and in the `ST_IDLE` no-IP watchdog path. Extra fields: `count`, `reason` (99 = `WIFI_REASON_BB_LOST_IP` sentinel), `retry_count`.
- `update_failure` (BB_ALERT_WARNING) in `components/bb_update_check/src/bb_update_check_common.c`: fired on fetch-failed and parse-failed paths (both streaming and custom parser). Extra fields: `current`, `latest`.
- `update_available` (BB_ALERT_INFO, transition edge only): fired when `available` transitions from `false` → `true`. Extra fields: `current`, `latest`.
- `update_applied` (BB_ALERT_INFO) in `bb_update_check_mark_check_on_apply()`: fired when boot-mode OTA marks status as `check_on_apply` (staged/pending-reboot). Extra fields: `current`, `latest`.

**Convention:** durable state → retained `bb_cache` topics; discrete one-shot events → `bb_alert`.

## Event bus (bb_event, bb_event_ring, bb_event_routes)

`bb_event` is a portable callback-list publish/subscribe event bus. On ESP-IDF, subscribers receive events via a FreeRTOS dispatcher task; on Arduino, the app pumps events from `loop()` via `bb_event_pump()`. `bb_event_ring` is a sibling component providing a circular buffer with replay-on-subscribe — designed for fan-out scenarios (SSE/WebSocket subscribers, persistent event history). Both use the same `bb_event_t` opaque handle and dispatch model; `bb_event_ring` layers replay semantics on top of `bb_event` internally.

`bb_event_routes` exposes `GET /api/events` as a Server-Sent Events stream. Producers call `bb_event_routes_attach("topic.name")` to surface a topic on the stream; the route fans out to multiple concurrent clients, each with its own replay-on-connect via `bb_event_ring`. The payload contract is **caller posts valid UTF-8 JSON** — the route emits it raw in the SSE `data:` field. Arduino targets ship a 503 stub today (CC3000 RAM constraints); ESP-IDF is the production path.

**`_ex` variants and the retained flag.** Both `bb_event_ring_attach` and `bb_event_routes_attach` have `_ex` variants that accept a `bool retained` parameter:
- `bb_event_ring_attach_ex(topic, capacity, max_entry, retained, out)` — preferred form; `bb_event_ring_attach` is `retained=false`.
- `bb_event_routes_attach_ex(topic_name, retained)` — preferred form; `bb_event_routes_attach` is `retained=false`.

Set `retained=true` for **state topics** (topics that publish current state, not discrete events). The flag documents intent and reserves API surface for future drain semantics. The practical effect today: callers that set `retained=true` MUST also publish an initial snapshot at component init so the ring is non-empty from T=0 — this ensures SSE clients connecting before the first periodic post receive the last known value rather than empty state.

**SSE client subscription filtering.** `GET /api/events?topic=<name>` filters incoming SSE stream to a specific topic. The filter is applied server-side at client acquire time: `bb_event_routes_client_acquire_ex(out, topic_filter)` subscribes only to topics matching the filter, saving dispatcher work and reducing per-client queue pressure. Pass `NULL` to subscribe to all attached topics (equivalent to `bb_event_routes_client_acquire`). ESP-IDF clients extract the query param and pass it to `bb_event_routes_client_acquire_ex`; if the filter doesn't match any attached topic, the client still acquires but receives only heartbeats.

**Auto-attach convention.** A component that emits a `bb_event` topic owns its own attach to `bb_event_routes`, gated by `CONFIG_BB_<NAME>_AUTO_ATTACH` (`depends on BB_EVENT_ROUTES_AUTOREGISTER`). Consumers enabling both components get the topic on `/api/events` without an explicit `bb_event_routes_attach()` call. Disable the flag if you want the topic available for internal subscribers only. Today `bb_update_check` follows this pattern (`update.available`) using `bb_event_routes_attach_ex(..., true)`; future emitters (`ota.progress`, `mining.share.*`) will follow the same shape. **Hardware-specific satellites** (`bb_pub_fan`, `bb_pub_power`, `bb_pub_thermal`, `bb_display_info`) default **n** — opt-in per board (enable only where the hardware exists, to avoid phantom topics on boards that lack the fan/power VRM/thermal sensors/display panel). **Universal/feature satellites** (`bb_pub_info`, `bb_pub_wifi`, `bb_update_check`) keep **default y**.

**Topic discovery and diagnostics.** `GET /api/diag/events` lists every topic currently attached to `/api/events` with ring-buffer diagnostics: `ring_capacity`, `ring_count` (0 → no replay possible), `last_id`, `last_post_age_ms` (age of the last ring entry in ms, computed at request time; 0 if no events captured), and `last_size`. Top-level fields `max_clients` and `active_clients` show the configured concurrency cap and how many SSE connections are live. This is the first step when SSE shows `: connected` but no replay data — confirm the topic is attached and `ring_count > 0` before investigating the producer.

**Platform split (ESP-IDF components).** `bb_event_ring` and `bb_event_routes` are portable components with no ESP-IDF dependencies in their `components/` CMakeLists. ESP-IDF platform implementations live in standalone components under `platform/espidf/`:
- `platform/espidf/bb_event_ring_espidf/` — provides `bb_event_ring_now_us()` backed by `esp_timer_get_time()`. REQUIRES `bb_event_ring esp_timer`.
- `platform/espidf/bb_event_routes_espidf/` — registers `/api/events` + `/api/diag/events` HTTP routes. REQUIRES `bb_event_routes` and all ESP-only deps (`bb_json`, `esp_http_server`).

ESP-IDF consumers must add both directories to `EXTRA_COMPONENT_DIRS` and include `bb_event_ring_espidf` and `bb_event_routes_espidf` in their component's `REQUIRES` list (or the app's `idf_component_register` REQUIRES). The portable `bb_event_ring` and `bb_event_routes` components declare no `esp_timer`, `esp_http_server`, or `bb_json` dependencies.

**Heap budgets and Kconfig defaults.** At defaults `BB_EVENT_ROUTES_MAX_CLIENTS=2` and `BB_EVENT_ROUTES_QUEUE_DEPTH=8`, each client uses ~2.7 KB (8×256 B payload buf + 8×24 B queue entry overhead + ~88 B mutex). Consumers with more heap can override via `sdkconfig`. See `components/bb_event_routes/Kconfig`.

**Dispatch pool and per-client queue allocation.** The dispatch pool and per-client queues prefer SPIRAM on ESP-IDF (overridden at platform init with fallback to default heap). `bb_event_ring` per-topic rings also use SPIRAM-preferred allocation on ESP-IDF.

**Tunables (from `#296`).** `BB_EVENT_MAX_TOPICS` default 8; `BB_MDNS_EVT_POOL_SIZE` default 8; `BB_DIAG_PANIC_BUF_SIZE` default 1024 (RTC slow). New: `BB_LOG_TAG_REGISTRY_MAX` (default 24), `BB_HTTP_ROUTE_REGISTRY_CAP` (default 64), `BB_MDNS_TXT_PENDING_MAX` (default 4), `BB_UPDATE_CHECK_CUSTOM_PARSER_BUF_BYTES` (default 8192).

## Update check (bb_update_check, bb_release_manifest, bb_http_client)

`bb_update_check` periodically polls a release-manifest URL, compares the returned version against `bb_system_get_version()`, and on a state change posts the `update.available` `bb_event` topic and updates the mDNS TXT key `update=<value>` (`unknown` pre-check, `none` up to date, `<tag>` when newer is available).

- **Initial snapshot:** `bb_update_check_init` does NOT post one; callers must call `bb_update_check_publish_initial()` after attaching a ring/routes with `retained=true`. The ESP-IDF platform impl (`bb_update_check_register_init`) does this automatically under `CONFIG_BB_UPDATE_CHECK_AUTO_ATTACH`. Initial snapshot: `available=false`, `current=<running version>`, `latest=""`, `download_url=""`, `last_check_ok=false`.
- **Parser:** `bb_update_check_set_releases_url(url)` + optionally `bb_update_check_set_parser(fn)`. Default parser is `bb_release_manifest_parse_github`; custom parsers supply `bb_release_manifest_parse_fn`.
- **Transport:** fetches via `bb_http_client` (same wrapper as `bb_ota_pull`).
- **Routes:** auto-registers `GET /api/update/status`; ESP-IDF only — Arduino stubs every setter to `BB_ERR_UNSUPPORTED`.

**Runtime opt-out.** `bb_nv_config_update_check_enabled()` / `bb_nv_config_set_update_check_enabled(bool)` mirror the `_mdns_enabled` pattern: the getter returns `true` by default; the setter persists to NVS on ESP-IDF (in-memory only on host). The periodic timer still fires on its interval, but `bb_update_check_run_one` no-ops and returns `BB_OK` when the flag is `false`. `GET /api/update/status` includes an `"enabled"` boolean field that reflects the current NV setting. The compile-time `CONFIG_BB_UPDATE_CHECK_AUTOREGISTER` Kconfig is unchanged; both compile-time and runtime opt-outs are independent.

**Firmware board name.** `bb_update_check_set_firmware_board(board)` overrides the asset name prefix used when matching release assets. Default fallback is `"unknown"` (looks for `unknown.bin`). Pass the board prefix without `.bin` (e.g. `"taipanminer-tdongle-s3"` → `taipanminer-tdongle-s3.bin`). Pass `NULL` or `""` to revert to the default. `bb_update_check_get_status().board` is the single source of truth for the effective board name. `bb_ota_pull` reads from `bb_update_check_get_status()` — **`bb_ota_pull_set_firmware_board` has been removed**; use `bb_update_check_set_firmware_board` only. Returns `BB_ERR_INVALID_STATE` before init, `BB_ERR_INVALID_ARG` if string exceeds 63 chars.

**Streaming fetch path.** The default GitHub parser is invoked via the streaming API: `bb_http_client_get_stream` feeds 2 KB chunks into `bb_release_manifest_parse_github_stream_feed`. Only parser state (~400 B on stack) and the two extracted strings live in memory. Custom parsers registered via `bb_update_check_set_parser` fall back to a heap allocation per fetch (size tunable via `CONFIG_BB_UPDATE_CHECK_CUSTOM_PARSER_BUF_BYTES`, default 8192 bytes). `bb_ota_pull` uses `bb_update_check_get_status()` for the cached result; `ota_fetch_manifest` is still compiled for the `BB_OTA_PULL_TESTING` hook but is not called on device.

**`bb_update_check` is the single source of truth for manifest checks.** `bb_update_check_get_status(bb_update_check_status_t *out)` copies the cached status snapshot under the internal pthread mutex. Callers do not hold any lock. `POST /api/update/apply` reads this to extract `latest` and `download_url` for the OTA worker; returns 503 if `last_check_ok == false` (no recent successful check), 409 if `available == false`. `POST /api/update/check` calls `bb_update_check_kick()` and returns `{"status":"checking"}` (200) immediately; callers poll `GET /api/update/status` for results.

**`bb_update_check_now()` vs `bb_update_check_kick()`:**
- `now()` — synchronous; runs manifest fetch + mbedTLS on caller's stack (needs ≥8 KB). Use from worker tasks and test harnesses.
- `kick()` — non-blocking on ESP-IDF; posts semaphore to wake worker task. Use from HTTP handlers (httpd workers ~4 KB). On host/Arduino, synchronous stub.

The streaming parser (`bb_release_manifest_parse_github_stream_{begin,feed,end}`) is a resumable byte-at-a-time state machine. Matches `assets[].name == "<board_fallback>.bin"` → takes that asset's `browser_download_url`.

**Pause/resume hooks.** `bb_update_check_set_hooks(pause_fn, resume_fn)` brackets each manifest fetch. `pause_fn` is called just before `bb_http_client_get_stream`; `resume_fn` immediately after (both success and failure). Mirrors `bb_ota_pull_set_hooks`. Callback types are `bb_http_pause_cb_t` / `bb_http_resume_cb_t` from `bb_core.h`. Either argument may be NULL.

**Task stack budget.** Any task that calls `bb_http_client_get*` must allocate at least `BB_HTTP_CLIENT_TASK_STACK` bytes (8 KiB default, `CONFIG_BB_HTTP_CLIENT_TASK_STACK_SIZE`). The mbedTLS handshake + cert-bundle parse path needs 5–8 KiB; a smaller task stack overflows into adjacent heap blocks and corrupts heap metadata, surfacing as an unrelated assertion in the next `calloc`. Both `bb_update_check` and `bb_ota_pull` reference the macro so the budget stays consistent.

## Provisioning UI

The `bb_prov` component manages the provisioning state machine and HTTP `/save` handler. Callers MUST supply at least one asset with `path="/"` to `bb_prov_start`. For bare-minimum bringup, add `REQUIRES bb_prov_default_form` and pass `&bb_prov_default_form_asset`. Custom UIs pass their own asset array instead. `POST /save` returns `204 No Content`; the caller's form JS is responsible for post-submit UX.

`bb_prov_start(assets, n, extra)` owns the full prov-mode route graph: it registers `/save`, assets, registry routes (e.g. `POST /api/scan`, `/api/reboot`, `/api/info`), an optional consumer `extra` callback, then the captive-portal `/*` GET wildcard — in that exact order so specific handlers always win first-match. Pass `NULL` for `extra` when the UI only needs the built-ins. Use `extra` for advanced UIs that need dynamic endpoints (e.g. live diagnostics, pool-test buttons); register them on `server` and bb_prov will sequence them correctly. Additional form fields (pool/wallet/worker/etc.) stay on the `/save` body and are parsed via `bb_prov_set_save_callback`.

## Display

**Two display tiers — chosen by panel class, not preference.** `bb_display`'s core API is portable pixel-blit primitives (clear/blit/flush). Small SPI/I2C panels (st77xx, ili9341, ssd1306) implement it directly and pull in **no** UI framework. Large MIPI-DSI / RGB framebuffer panels (ek79007) are instead driven via **LVGL** (`esp_lvgl_port`) and additionally hand the consumer an `lv_obj_t *` screen root (e.g. `bb_display_ek79007_screen()`) so it can build rich widget UIs. That LVGL coupling is **deliberate and ESP-IDF-only** — hand-blitting a 1024x600 UI is impractical, and the `lv_obj_t` handoff is the agreed escape hatch, **not** a portability leak (so `esp_lvgl_port` stays in that backend's public `REQUIRES`). New big-panel backends follow this shape; small panels must **not** pull LVGL — it doesn't fit a no-PSRAM, CPU-busy classic ESP32 (e.g. the CYD/2432S028R, which uses the blit path).

For LVGL-backed panels: LVGL is initialized inside `bb_display_init` when an LVGL backend is registered. The consumer's `sdkconfig` governs LVGL font availability (`CONFIG_LV_FONT_MONTSERRAT_*`) and color depth (must be 16-bit / RGB565); breadboard does not ship pre-defined dashboard layouts. Every call into LVGL from application code (including `lv_timer` callbacks not running on the LVGL task) must be wrapped in `bb_display_lock` / `bb_display_unlock`. The `bb_display` component is ESP-IDF-only.

`bb_display_spi_common` provides shared SPI helpers: `bb_display_spi_init_bus()`, `bb_display_blit_spi()`, `bb_display_clear_spi()`. ili9341 and st77xx share a bounce buffer via this component. Use `BB_DISPLAY_AUTOREGISTER(chip, kconfig_sym, backend_ptr)` instead of per-driver constructor boilerplate.

Per-font Kconfigs: `CONFIG_BB_DISPLAY_FONT_5X8`, `CONFIG_BB_DISPLAY_FONT_6X12`, `CONFIG_BB_DISPLAY_FONT_8X16` (all default y).

**`/api/info` display extender (`bb_display_info` satellite).** Add `REQUIRES bb_display_info` and call `bb_display_register_info()` before `bb_http_server_start` to register a `bb_info` extender that emits a nested `"display"` object:
- `present` (bool) — `bb_display_backend_name() != NULL`
- if present: `panel` (string), `width`/`height` (int), `enabled` (bool from `bb_nv_config_display_enabled()`)
- if absent: `{"present": false}`

Also contributes a JSON-Schema properties fragment to the `/api/info` 200 response schema via `bb_info_register_extender_ex`. Sources: `platform/espidf/bb_display_info/bb_display_info.c` (ESP-IDF) / `platform/host/bb_display_info/bb_display_info.c` (host).

## LED

`bb_led` is a portable multi-handle LED API. `bb_led_set_primary(h)` / `bb_led_primary()` record a single app-level primary/status LED handle for introspection; `bb_led_name(h)` returns the driver's static name string (e.g. `"apa102"`, `"pwm"`, `"gpio"`).

**`/api/info` LED extender (`bb_led_info` satellite).** Add `REQUIRES bb_led_info` and call `bb_led_register_info()` before `bb_http_server_start` to register a `bb_info` extender that emits a nested `"led"` object from `bb_led_primary()`:
- `present` (bool) — `bb_led_primary() != NULL`
- if present: `type` (string, `bb_led_name`), `count` (int), `rgb` (bool, `BB_LED_CAP_RGB`), `enabled` (bool, `bb_led_enabled(primary)`)
- if absent: `{"present": false}`

`bb_led_enabled(h)` / `bb_led_set_enabled(h, bool)` — consumer-controlled logical on/off flag stored on the handle (default `true`). Does **not** change LED hardware; it is a reported state field consumers set to reflect logical LED state (e.g. heartbeat disabled). Existing consumers that never call `bb_led_set_enabled` see `enabled:true` in the info response.

Also contributes a JSON-Schema properties fragment via `bb_info_register_extender_ex`. Source: `platform/host/bb_led_info/bb_led_info.c`.

**`/api/health` temperature extender (`bb_temp` satellite).** Add `REQUIRES bb_temp` and call `bb_temp_register_info()` before `bb_http_server_start` to register a `bb_health` extender that emits a nested `"temp"` object on `/api/health`:
- `present` (bool) — SoC internal temperature sensor supported and readable (`bb_temp_read_soc` succeeded)
- if present: `soc_c` (number, die temperature in Celsius, 1 decimal place)
- if absent: `{"present": false}`

Sensor availability: ESP32-S2/S3/C3/C6/H2 and later have the modern `temperature_sensor` peripheral. Classic ESP32 (WROOM-32) does NOT — `present` is always `false` on that target. The component compiles cleanly on all targets via `#if SOC_TEMP_SENSOR_SUPPORTED` in `bb_system`. Also contributes a JSON-Schema properties fragment to the `/api/health` 200 response schema via `bb_health_register_extender_ex`. Sources: `platform/espidf/bb_temp/bb_temp.c` (ESP-IDF) / `platform/host/bb_temp/bb_temp.c` (host, test-injectable via `BB_TEMP_TESTING` + `bb_temp_test_set_soc`).

**`/api/health` MQTT extender (`bb_mqtt_info` satellite).** Add `REQUIRES bb_mqtt_info` and call `bb_mqtt_register_health()` before `bb_http_server_start` to register a `bb_health` extender that emits a nested `"mqtt"` object on `/api/health`:
- `enabled` (bool) — `bb_mqtt_default() != NULL` (MQTT was configured and started)
- `connected` (bool) — `bb_mqtt_is_connected(bb_mqtt_default())`

Also contributes a JSON-Schema properties fragment to the `/api/health` 200 response schema via `bb_health_register_extender_ex`. Sources: `platform/espidf/bb_mqtt_info/bb_mqtt_info.c` (ESP-IDF) / `platform/host/bb_mqtt_info/bb_mqtt_info.c` (host).

**`/api/health` network-health extender + retained SSE topic (`bb_net_health`).** Adds `REQUIRES bb_net_health` and calls in two steps:
1. `bb_net_health_register_health()` — before `bb_http_server_start` (before the health section table is frozen). Registers a `"net"` section on `/api/health` with fields: `rssi` (int), `state` (string: `"good"` / `"marginal"` / `"poor"`), `early_warning` (bool), `throttled` (bool), `last_disconnect_reason` (uint, WiFi), `disc_age_s` (uint), a nested `"mqtt"` object: `{ "connected": bool, "reconnect_count": uint, "disc_age_s": uint, "disc_reason": uint, "tls_fail": uint }`, and a nested `"http"` object: `{ "connected": bool, "consec_failures": uint, "tls_fail": uint, "last_status": int }` sourced from `bb_sink_http_get_health()`. The raw mbedtls error (`bb_http_client_result_t.tls_error_code`) is classified to `tls_fail` and logged inside `bb_sink_http`; it is never forwarded to net.health or JSON.
2. `bb_net_health_attach_sse()` — in the regular-tier init (after `bb_event_routes` is initialised). Attaches the `"net.health"` retained SSE topic, publishes an initial snapshot so the ring is non-empty from T=0, and starts a 5-second periodic evaluator that re-publishes on state or `early_warning` change.

**Breaking change (B1-362):** `mqtt_connected` and `mqtt_reconnect_count` have moved from flat top-level fields into a nested `"mqtt"` sub-object. `last_disconnect_reason` is now the WiFi disconnect reason (`wi->disc_reason`), not the MQTT reason. MQTT disconnect diagnostics live in `mqtt.disc_reason` (bb_mqtt_disc_t) and `mqtt.tls_fail` (bb_tls_fail_t).

**Pure classifier** (`bb_net_health_eval`): RSSI buckets: GOOD ≥ −67 dBm, MARGINAL −75..−68 dBm, POOR < −75 dBm. Hysteresis: 3 consecutive worse samples before downgrade, 3 better samples before upgrade (no single-sample flap). `early_warning` true when: state is POOR, or `mqtt_reconnect_count` increased since last eval, or MQTT disconnected within 60 s. Host-testable, 100% branch coverage. Sources: `components/bb_net_health/src/bb_net_health.c` (pure, host + device) / `platform/espidf/bb_net_health/bb_net_health.c` (ESP-IDF glue). Adaptive backoff Kconfig knob in commit 4 (`BB_PUB_ADAPTIVE_BACKOFF`); see `components/bb_net_health/Kconfig`.

**`bb_mqtt_stats_t` + `bb_mqtt_get_stats(h, out)`** (added to `bb_mqtt`): snapshot of `reconnect_count`, `connected`, `disc_reason` (bb_mqtt_disc_t, replaces `last_disc_error_type`), `tls_fail` (bb_tls_fail_t), and `tls_error_code` (raw mbedtls int, log-only — NOT emitted in JSON) read atomically under `h->lock`. Host stub supports `bb_mqtt_host_simulate_reconnect`, `bb_mqtt_host_set_disc_reason`, and `bb_mqtt_host_set_tls_fail` test hooks.

**`CONFIG_BB_MQTT_NETWORK_TIMEOUT_MS`** (default 30 000 ms, range 1 000–120 000): maps to `esp_mqtt_client_config_t.network.timeout_ms`. Prevents esp-mqtt from aborting in-flight writes on a marginal link and churning reconnects.

## Power (`bb_power`, `bb_power_tps546`, `/api/power`)

`bb_power` is a portable voltage-regulator monitor HAL. `bb_power_set_primary(h)` / `bb_power_primary()` record a single app-level primary power handle. `bb_power_poll(h)` reads all channels via the vtable and caches the result (mutex-protected). `bb_power_snapshot(h, &out)` returns the cached reading (mutex-protected); all fields are -1 if h is NULL or readings errored.

**Poll/snapshot model.** The consumer calls `bb_power_poll` periodically (e.g. once per second from a monitoring task) and the HTTP handler calls `bb_power_snapshot` to read the cache — the two paths are decoupled and thread-safe via a `pthread_mutex_t` (POSIX on host; ESP-IDF ships a POSIX pthread layer).

**`bb_power_snapshot_t` fields:**
- `vout_mv`, `iout_ma`, `vin_mv`, `temp_c` — -1 if unavailable or read error
- `pout_mw` — `(vout_mv * iout_ma) / 1000` when both ≥0; else -1 (raw, no board offset)

**`bb_power_tps546` backend (ESP-IDF only).** `bb_power_tps546_open(cfg, &h)` adds the device to an I2C bus, runs the full TPS546 protection init sequence (AxeOS default-family parity), and calls `bb_power_handle_create`.

Config `bb_power_tps546_cfg_t`: `{ bus, addr, target_mv, switch_freq_khz, oc_limit_a, oc_response, protect }` — zero values in the outer fields use defaults (650 kHz, 30 A, 0xC0). The `protect` sub-struct (`bb_power_tps546_protect_t`) carries the full protection and soft-start config; **every field defaults to 0 = skip that write**, so existing callers that don't populate `protect` get unchanged behaviour.

**`cfg.protect` fields** (all 0 = skip; VOUT factors multiplied by `target_mv/1000` at encode time):
- `vin_on_v`, `vin_off_v`, `vin_uv_warn_v`, `vin_ov_fault_v` — VIN thresholds (V, SLINEAR11 float)
- `vin_ov_fault_response` — VIN OV fault response byte (e.g. 0xB7)
- `vout_scale_loop` — VOUT_SCALE_LOOP factor (SLINEAR11 float; e.g. 0.25)
- `vout_max_v`, `vout_min_v` — absolute VOUT clamps (V, ULINEAR16)
- `vout_ov_fault_factor`, `vout_ov_warn_factor` — OV limits = factor × target_V (ULINEAR16)
- `vout_margin_high`, `vout_margin_low` — margin limits = factor × target_V (ULINEAR16)
- `vout_uv_warn_factor`, `vout_uv_fault_factor` — UV limits = factor × target_V (ULINEAR16)
- `vout_uv_fault_response` — UV-fault response byte; 0xBF = hiccup/auto-restart (continuous retry) vs default latch-off; 0 = skip
- `iout_oc_warn_a` — IOUT OC warn limit (A, SLINEAR11 float)
- `ot_warn_c`, `ot_fault_c` — OT limits (°C, SLINEAR11 int)
- `ot_fault_response` — OT fault response byte (e.g. 0xFF)
- `ton_delay_ms`, `ton_rise_ms`, `ton_max_fault_ms` — soft-start timing (ms, SLINEAR11 int)
- `ton_max_fault_response` — TON max fault response byte
- `on_off_config`, `stack_config`, `sync_config`, `phase` — topology registers
- `compensation_config[5]` — COMPENSATION_CONFIG block (written iff any byte non-zero)

**Pure builder (host-testable).** `bb_power_tps546_build_init_program(cfg, vout_exp, out, max)` returns the ordered PMBus write list as `bb_tps546_write_t` entries in AxeOS `write_entire_config()` order. No I2C; compiled on both host and ESP-IDF. The espidf executor walks the program and issues byte/word/block writes, then CLEAR_FAULTS + OPERATION_ON.

Encode helpers in `tps546_decode.h` (pure, host-testable): `tps546_float_2_slinear11`, `tps546_int_2_slinear11`, `tps546_float_2_ulinear16`.

PMBus decode math also in `tps546_decode.h` (no ESP-IDF; host-testable).

**`/api/power` route (`bb_power_routes`, opt-in).** Route registration is **opt-in** via `CONFIG_BB_POWER_ROUTES_AUTOREGISTER` (default **n**). When enabled, `bb_power_routes_init` auto-registers at order 1; disable it and call `bb_power_routes_init` manually to control registration timing, or omit the call entirely if your app defines its own `/api/power` handler. Emits a JSON object from `bb_power_primary()`:
- `present` (bool) — `bb_power_primary() != NULL`
- `vout_mv`, `iout_ma`, `pout_mw`, `vin_mv`, `temp_c` — number or null (when -1 or not present)
- then `bb_http_route_run_extenders("power", root)` — satellites (e.g. TM) add fields like `pcore_mw`

**Route-extender ordering.** Satellites register "power" extenders at order 0 (regular tier). `bb_power_routes_init` runs at order 1 and calls `bb_http_route_assemble_schema("power", base, suffix)` to build the published schema. `bb_http_extender_freeze()` is called by `bb_info_init` (order 2); no double-freeze.

**Schema.** `s_power_responses[0].schema` is set at `bb_power_routes_init` time to the assembled string (base fields + extender fragments + required array).

**Sources:**
- `components/bb_power/` — portable core (header + CMakeLists + Kconfig)
- `platform/host/bb_power/bb_power.c` — shared core (compiled host + espidf)
- `components/bb_power_tps546/` — TPS546 component (headers + CMakeLists)
- `platform/espidf/bb_power_tps546/bb_power_tps546.c` — ESP-IDF open/exec
- `platform/host/bb_power_tps546/bb_power_tps546_program.c` — pure builder (compiled host + espidf)
- `components/bb_power_routes/` — route component
- `platform/espidf/bb_power_routes/bb_power_routes.c`
- `platform/host/bb_power_routes/bb_power_routes_host.c` (test hooks)

## Fan (`bb_fan`, `bb_fan_emc2101`, `/api/fan`)

`bb_fan` is a portable fan-controller + temperature HAL. `bb_fan_set_primary(h)` / `bb_fan_primary()` record a single app-level primary fan handle. `bb_fan_poll(h)` reads all channels via the vtable and caches the result (mutex-protected). `bb_fan_snapshot(h, &out)` returns the cached reading (mutex-protected); integer fields are -1 and float fields are NAN if h is NULL or readings errored.

**Poll/snapshot model.** Consumer calls `bb_fan_poll` periodically (e.g. once per second) and the HTTP handler calls `bb_fan_snapshot` — the two paths are decoupled and thread-safe via `pthread_mutex_t`.

**`bb_fan_snapshot_t` fields:**
- `rpm` — -1 if unavailable or stalled (tach = 0xFFFF)
- `duty_pct` — -1 if not yet set or unknown
- `die_c` — NAN if external diode read failed or not supported
- `board_c` — NAN if internal sensor read failed or not supported

**`bb_fan_emc2101` backend (ESP-IDF only).** `bb_fan_emc2101_open(cfg, &h)` adds the device to an I2C bus, runs the EMC2101 init sequence (config register, fan config register, optional ideality/beta compensation writes, failsafe 100% duty), and calls `bb_fan_handle_create`. Config: `{ bus, addr, ideality, beta }` — `ideality`/`beta` = 0 skips those register writes (e.g. bitaxe-403). Pure decode math (ext temp 11-bit 0.125°C, internal temp 8-bit 1°C, RPM = 5400000/tach) lives in `components/bb_fan_emc2101/include/emc2101_decode.h` (pure, no ESP-IDF; host-testable).

**`/api/fan` route (`bb_fan_routes`, opt-in).** Route registration is **opt-in** via `CONFIG_BB_FAN_ROUTES_AUTOREGISTER` (default **n**). When enabled, `bb_fan_routes_init` auto-registers at order 1; disable it and call `bb_fan_routes_init` manually to control registration timing, or omit the call entirely if your app defines its own `/api/fan` handler. GET emits a JSON object from `bb_fan_primary()`:
- `present` (bool) — `bb_fan_primary() != NULL`
- `rpm`, `duty_pct` — number or null (when -1 or not present)
- then `bb_http_route_run_extenders("fan", root)` — satellites add fields

When `CONFIG_BB_FAN_AUTOFAN=y`, GET also emits: `die_ema_c`, `vr_ema_c`, `pid_input_c`, `pid_input_src` (same field names as TM's `/api/fan`).

POST behavior:
- Without `BB_FAN_AUTOFAN`: `{"duty_pct": N}` (0..100) sets raw duty; returns 200 `{"status":"ok","duty_pct":N}`, 400 on bad input, 503 if no primary.
- With `BB_FAN_AUTOFAN`: parses autofan config fields — `autofan` (bool), `die_target_c`, `vr_target_c`, `manual_pct`, `min_pct` (all optional, partial update); returns 204 on success.

**Autofan PID (`CONFIG_BB_FAN_AUTOFAN`, default n).** Opt-in PID controller that runs inside `bb_fan_poll()` when enabled. Faithful port of TM's proven autofan: Kp=5 Ki=0.1 Kd=2, P_ON_E, REVERSE direction, 5000 ms sample time, dual EMA (alpha=0.2) for die and aux temps. Input source selected by max((ema−target)/target) ratio. Config via `bb_fan_set_autofan(h, cfg)`; aux (VR) temp fed by consumer via `bb_fan_set_aux_temp(h, c)` (bb_fan does not depend on bb_power). Telemetry via `bb_fan_get_autofan_telemetry(h, &tel)`. On ESP-IDF, call `bb_fan_autofan_inject_clock(h)` once after handle creation to inject the esp_timer clock (required for correct 5s gating). **BB fully owns fan duty in both modes when compiled in:** PID when `enabled=true`; `manual_pct` (clamped 0..100) applied each poll when `enabled=false` — consumer never calls `bb_fan_set_duty_pct()` for steady-state control. When the feature is compiled OUT (default), `bb_fan_poll()` does not change duty. `GET /api/fan` emits `pid_input_src` as `"die"` or `"vr"` (wire names; internal C uses `"aux"` generically). POST fires the persist callback (if registered via `bb_fan_routes_set_autofan_persist_cb`) only from the HTTP path, not from direct `bb_fan_set_autofan()` calls.

**Route-extender ordering.** Satellites register "fan" extenders at order 0. `bb_fan_routes_init` runs at order 1 and calls `bb_http_route_assemble_schema("fan", base, suffix)`. `bb_http_extender_freeze()` is called by `bb_info_init` (order 2).

**Sources:**
- `components/bb_fan/` — portable core (header + CMakeLists + Kconfig)
- `components/bb_fan/src/bb_fan_pid.c` — vendored PID (MIT; compiled when BB_FAN_AUTOFAN=y)
- `platform/host/bb_fan/bb_fan.c` — shared dispatch (compiled host + espidf)
- `platform/espidf/bb_fan/bb_fan_clock_espidf.c` — esp_timer clock shim (espidf + BB_FAN_AUTOFAN)
- `components/bb_fan_emc2101/` — EMC2101 backend (espidf-only)
- `platform/espidf/bb_fan_emc2101/bb_fan_emc2101.c`
- `components/bb_fan_routes/` — route component
- `platform/espidf/bb_fan_routes/bb_fan_routes.c`
- `platform/host/bb_fan_routes/bb_fan_routes_host.c` (test hooks)

## Thermal aggregate (`bb_thermal`, `/api/thermal`)

`bb_thermal` is a route-only component (no HAL, no backend) that aggregates temperatures from `bb_temp` (SoC), `bb_power` (VR), and `bb_fan` (ASIC die + board) into a single `/api/thermal` endpoint. It is opt-in by REQUIRES — a consumer wanting the aggregate thermal view adds `REQUIRES bb_thermal`. Route registration is also **opt-in** via `CONFIG_BB_THERMAL_AUTOREGISTER` (default **n**). When enabled, `bb_thermal_init` auto-registers at order 1; disable it and call `bb_thermal_init` manually to control registration timing, or omit the call entirely if your app defines its own `/api/thermal` handler.

**`GET /api/thermal`** emits four source objects, each `{present, c}` (c = number °C, or null when not present):

```json
{ "soc":   {"present": bool, "c": number|null},
  "vr":    {"present": bool, "c": number|null},
  "asic":  {"present": bool, "c": number|null},
  "board": {"present": bool, "c": number|null} }
```

- `soc`: `bb_temp_read_soc(&c)` → present = (rc == true).
- `vr`: `bb_power_snapshot(bb_power_primary())` → present = primary != NULL && temp_c >= 0; c = temp_c.
- `asic`: `bb_fan_snapshot(bb_fan_primary())` → present = primary != NULL && !isnan(die_c); c = die_c.
- `board`: same fan snapshot → present = primary != NULL && !isnan(board_c); c = board_c.
- then `bb_http_route_run_extenders("thermal", root)` — satellites (e.g. TM) can add mining-context temps.

**Route-extender ordering.** Satellites register "thermal" extenders at order 0. `bb_thermal_init` runs at order 1 and calls `bb_http_route_assemble_schema("thermal", base, suffix)`. `bb_http_extender_freeze()` is called by `bb_info_init` (order 2).

**Sources:**
- `components/bb_thermal/` — route component (header + CMakeLists + Kconfig)
- `platform/espidf/bb_thermal/bb_thermal.c`
- `platform/host/bb_thermal/bb_thermal_host.c` (test hooks)

## Portable timing

`bb_clock_now_ms()` in `bb_core/include/bb_clock.h` provides a portable millisecond timestamp. Named timing constants live in their respective headers: `BB_BUTTON_DEBOUNCE_MS_DEFAULT`, `BB_BUTTON_EVENTS_*_DEFAULT_MS`, `BB_LED_ANIM_*_DEFAULT_MS`. `bb_timer` also exposes `bb_timer_now_us()` for microsecond timestamps.

## LED animation (bb_led_anim)

`bb_led_anim` drives patterns (SOLID/BLINK/BREATHE/PULSE/COLOR_CYCLE/CHASE) on a `bb_led` handle off a `bb_timer` (or `bb_led_anim_tick()` on Arduino). Brightness patterns interpolate at 16-bit and emit via `bb_led_set_level` so the driver's CIE gamma renders a smooth dim sweep.

- **Pattern transitions.** `bb_led_anim_set_transition(h, pat, fade_ms)` cross-fades from the current output level into the new pattern over `fade_ms` (e.g. a boot solid 50% fading into a breathe heartbeat). The fade is a brightness lerp in perceptual `set_level` space — **brightness/level patterns only** (solid, breathe, pulse, blink-with-level); RGB colour is not crossfaded. `bb_led_anim_set` is the instant (`fade_ms 0`) form.
- **Blink brightness.** `BB_ANIM_BLINK` gains `level_pct`: `0` keeps the legacy full on/off (`BB_LED_CAP_ONOFF`); `>0` flashes at that brightness via `set_level` (gamma-correct, needs `BB_LED_CAP_BRIGHTNESS`).

## bb_hw

Board pin/peripheral headers are consumer-supplied. Each firmware must provide its own board header file. In your build, set `-DBB_HW_BOARD_HEADER="<name>.h"` and ensure that header is on the include path (via CMake include dirs or PlatformIO `build_flags`). `bb_hw.h` requires this define and will error if not provided.

Example (PlatformIO):
```
build_flags =
    -I${PROJECT_DIR}/board
    -DBB_HW_BOARD_HEADER=\"my_board.h\"
```

Examples in this repo (elecrow-p4-hmi7, esp32-wroom-32) own their own board headers under `examples/<name>/board/`. bb_hw component provides no bundled board definitions.

## OTA strategy (`/api/update/apply` owner)

OTA update strategy is a mutually-exclusive Kconfig **choice** `BB_OTA_STRATEGY` (in `bb_core/Kconfig`, always sourced): `BB_OTA_STRATEGY_PULL` (default) | `BB_OTA_STRATEGY_BOOT` | `BB_OTA_STRATEGY_NONE`. The choice drives the defaults of `CONFIG_BB_OTA_PULL_AUTOREGISTER` (`default y if BB_OTA_STRATEGY_PULL`) and `CONFIG_BB_OTA_BOOT_AUTOREGISTER` (`default y if BB_OTA_STRATEGY_BOOT`), so **`POST /api/update/apply` has exactly one owner** — never both. `bb_ota_boot` links on every board but its route + force-register compile out unless the strategy is boot-mode.

`POST /api/update/apply` returns a strategy-specific 202 `status`:
- in-place pull (`bb_ota_pull`) → `update_started` — download proceeds at runtime heap; poll `GET /api/update/progress`.
- boot-mode (`bb_ota_boot`) → `rebooting_for_boot_mode_ota` — device reboots immediately and pulls at full early-boot heap. When `CONFIG_BB_OTA_BOOT_PROGRESS_HTTP=y`, the board serves a transient `GET /api/update/progress` during the boot-OTA window (same `{state,in_progress,progress_pct}` schema as the pull progress route) via a minimal httpd instance that exists only for the duration of the download; absent in normal operation. Without that config, the client waits for the device to reappear on a bumped version with no progress feed. Distinct from `bb_system`'s generic `rebooting` (`POST /api/reboot`, same firmware).

There is no `/api/update/boot` verb — boot-mode arms via `/api/update/apply`. The per-component autoregister bools stay user-visible for the manual-registration escape hatch.

**Strategy↔update-check coupling.** `CONFIG_BB_UPDATE_CHECK_AUTOREGISTER` defaults off when `BB_OTA_STRATEGY_BOOT` is selected: boot-mode boards run the manifest check synchronously inside the boot-mode worker at full early-boot heap, so the recurring runtime task (~8 KB worker + HTTPS timer) is dead weight and cannot complete a TLS handshake on heap-tight boards. Pull-strategy boards keep the default `y`. Override to `y` explicitly if a boot-mode board also needs the runtime `GET /api/update/status` route.

**`check_on_apply` directive (heap-tight boot-mode boards).** When `CONFIG_BB_OTA_BOOT_STATUS_HTTP=y` and the on-demand `POST /api/update/check` fires but heap is below the TLS threshold, the default is 503 `{"error":"insufficient_heap"}`. Enabling `CONFIG_BB_OTA_CHECK_ON_APPLY_FALLBACK=y` (depends on `BB_OTA_BOOT_STATUS_HTTP`, default n) changes this: instead of the 503, the handler calls `bb_update_check_mark_check_on_apply()` and returns 200 `{"status":"check_on_apply"}`. `GET /api/update/status` then reflects `outcome:"check_on_apply"`, `available:false`. This signals consumers to skip the runtime check and go straight to `POST /api/update/apply` — boot-mode always performs the full manifest check at early-boot heap where contiguous RAM is plentiful. The heap-OK path (real check → 202 `{"status":"checking"}`) is unchanged.

## bb_tls — shared TLS handshake-diag + heap-guard layer (B1-361)

`bb_tls` is a pure portable component (no ESP-IDF headers in `bb_tls.h`) providing two functions compiled on host and device:

- **`bb_tls_handshake_diag(mbedtls_err, host, ssl_in_len, out, out_len)`** — classifies a failed TLS handshake error and fills an actionable message. Returns `true` when the error matches the mbedtls record-size symptom (`-0x7200`), naming the endpoint HOST and `ssl_in_len` and suggesting `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` be increased.
- **`bb_tls_heap_guard_passes(largest_block, contiguous_floor, total_free, total_floor, out_dim)`** — pure predicate returning `true` when both heap dimensions clear their floors.

**Kconfig knobs (replacing per-component `BB_OTA_PULL_MIN_*` knobs — BREAKING in B1-361):**

| Kconfig | Default | Notes |
|---------|---------|-------|
| `CONFIG_BB_TLS_HEAP_CONTIGUOUS_FLOOR` | 0 | 0 = auto-derive (`BB_TLS_SSL_IN_FLOOR = SSL_IN + 1024`); >0 = explicit byte floor; <0 = disabled. |
| `CONFIG_BB_TLS_HEAP_TOTAL_FLOOR` | 0 | 0 = disabled; >0 = explicit byte floor for total-free check. |

**`CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` is the single knob — the floor auto-derives from it.** Set `BB_TLS_HEAP_CONTIGUOUS_FLOOR=0` (default) and lower `SSL_IN_CONTENT_LEN`; the floor follows automatically everywhere — `bb_ota_pull` and `bb_ota_boot` both derive from `BB_TLS_SSL_IN_FLOOR`. Do not hand-sync a separate guard value.

## OTA TLS pre-flight heap guard (bb_ota_pull, bb_ota_boot)

The guard checks `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)` before the TLS handshake and refuses the OTA with a clean error (`ESP_ERR_NO_MEM` / `check_on_apply`) if the largest contiguous internal block is below the floor — preventing an OOM crash mid-handshake on a fragmented or low-heap no-PSRAM board. Gates **both** the in-place pull (`bb_ota_pull`) and the boot-status `POST /api/update/check` (`bb_ota_boot`).

**Best-effort, two-stage.** Inside `bb_ota_pull`, the shared `ota_heap_guard()` helper runs at two points: `pre-flight` (function entry, before the wifi check + PM lock) and `pre-handshake` (PM lock held, immediately before `esp_https_ota_begin`). The second sample narrows — but cannot close — the window in which concurrent httpd/SSE/wifi can fragment internal heap between the check and the handshake; the guard is a clean-refusal heuristic, not a hard guarantee. Both stages share one log/`last_error` format tagged with the stage name.

`CONFIG_BB_TLS_HEAP_CONTIGUOUS_FLOOR` is the shared override: **0 (default) = auto-derive** from `SSL_IN_CONTENT_LEN`; a positive value pins an explicit byte floor; a negative value disables the guard. The guard is inert on boards with ample heap headroom (PSRAM boards, and pull-strategy boards after the OTA pause-hooks free the MQTT/ring heap).

**Runtime handshake diagnostic (B1-358/B1-361).** When `esp_https_ota_begin()` fails after exhausting inner retries, `bb_tls_handshake_diag()` (pure, no ESP-IDF deps, in `bb_tls`) emits an actionable log message naming the endpoint HOST extracted from `download_url` and the current `BB_TLS_SSL_IN_LEN` (bridged from `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN`). The message is also captured into `last_error` via `ota_set_error()` and surfaced by `GET /api/update/progress`. `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` → cert chain relationship: GitHub requires ≥ 16384 (default); other endpoints (CDN, AWS IoT) may work with 4096. `bb_ota_pull` does NOT enforce a floor on `SSL_IN_CONTENT_LEN` — the consumer firmware owns that value.

## OTA push body cap

`POST /api/update/push` enforces a body size limit via `CONFIG_BB_OTA_PUSH_MAX_SIZE` (default 4 MB). Requests exceeding the limit return 413 before any flash write begins.

**Error taxonomy (B1-307).** All failure paths return a JSON `{"error": "..."}` body with a distinct HTTP status; a closed connection is never the error signal. Status codes and their meanings: 400 = board mismatch or bad Content-Length; 408 = transfer too slow or timed out (retry with a better connection); 413 = payload too large; 422 = image received successfully but `esp_ota_end` SHA validation failed — incomplete or corrupt upload, safe to retry the push; 500 = internal failure (write error, malloc, no OTA partition, or set-boot-partition failed).

## GET /api/info memory regions

`GET /api/info` includes three nested memory-region objects (additive; back-compat `free_heap` and `heap_minimum_ever` flat fields unchanged):

- `heap_internal` `{free, total}` — `MALLOC_CAP_INTERNAL` (FreeRTOS/regular SRAM heap).
- `heap_psram`    `{free, total}` — `MALLOC_CAP_SPIRAM`; both 0 on no-PSRAM boards.
- `rtc`           `{used, total}` — RTC slow memory (static, not a heap). `total` = `SOC_RTC_DATA_HIGH − SOC_RTC_DATA_LOW` (8192 bytes on ESP32-S3). `used` = span of linker sections `_rtc_data_{start,end}`, `_rtc_bss_{start,end}`, `_rtc_noinit_{start,end}`, `_rtc_force_slow_{start,end}`. Host stubs return 0/0 for all three.

New `bb_board` accessors: `bb_board_heap_internal_{free,total}()`, `bb_board_psram_{free,total}()`, `bb_board_rtc_{used,total}()`.

## bb_diag

`bb_diag` registers diagnostic HTTP routes under `/api/diag/`: boot anomaly summary, panic log, coredump, heap stats, FreeRTOS task list, and abnormal-reset counter. `GET /api/diag/boot` returns a compact JSON summary of the current boot's reset reason, the persistent abnormal-reset count, and panic availability. `DELETE /api/diag/boot` clears both the panic log and the abnormal-reset counter in a single call. `GET /api/diag/panic` returns the full panic log detail (log tail, coredump backtrace, task info). Panic buffer size is tunable via `BB_DIAG_PANIC_BUF_SIZE` (default 1024 bytes, stored in RTC slow memory). `GET /api/diag/sockets` dumps LWIP TCP state distribution (per-state counts + per-PCB detail) for diagnosing socket exhaustion.

**Abnormal-reset counter semantics.** `abnormal_reset_count` (surfaced as `wdt_resets` in consumers) counts abnormal resets *since this firmware was deployed*, not a lifetime total. At each boot, the running app's ELF SHA256 is fingerprinted (first 4 bytes of the raw SHA256, stored in NVS key `app_fp` under the `bb_diag` namespace). If the stored fingerprint is absent (`0`) or differs from the running firmware, the counter is reset to 0 and the new fingerprint is stored — the deploy boot itself is the clean baseline and is not counted. Subsequent abnormal resets on the same build increment as before. Still explicitly clearable via `DELETE /api/diag/boot`. The decision logic is factored into the pure host-testable function `bb_diag_reset_decision()` in `components/bb_diag/bb_diag_reset_decision.c`.

## bb_version — build-time firmware version identifier

`scripts/bb_version.py` is a PlatformIO pre-script that writes a generated C header at
`<PROJECT_DIR>/.breadboard/gen/bb_version_gen.h` containing `#define BB_FW_VERSION_STR "<string>"`.
`bb_system_get_version()` returns `BB_FW_VERSION_STR` when the header is present (via
`#if __has_include("bb_version_gen.h")`), otherwise falls back to `esp_app_desc.version`.
The header is only rewritten when the content changes, preventing spurious recompiles.

**Precedence (highest → lowest):**
1. `BB_FW_VERSION` env var non-empty → used verbatim (override for CI/release pipelines)
2. Consumer repo has an exact git tag at HEAD → use that tag (release builds)
3. Dev default: `dev-<tm-ref>-<bb-ref>`
   - `tm-ref`: `main` on the main branch, else the 7-char short sha; a `+<hash4>` suffix (hash of the uncommitted `git diff`) marks a dirty tree so two dirty checkouts at the same sha stay distinguishable.
   - `bb-ref`: `bb-<pin>` when `.breadboard` is a pinned fetch (version derivable from the consumer commit's pin), or `bb-main`/`bb-<sha>[+hash4]` when `.breadboard` is a local symlink (floating dev checkout).
   - examples: `dev-main-bb-0.70.3` · `dev-main-bb-main` · `dev-806bf94+a1b2-bb-main`
   - consumer repo = `$PROJECT_DIR`; breadboard repo = resolved from the script's real path. Exact commit identity for crash decoding comes from the panic `app_sha256` + elf archive, not this human-readable string.

**Wiring a consumer (PlatformIO):**
```ini
extra_scripts = pre:.breadboard/scripts/bb_version.py
```
The script appends `.breadboard/gen` to `CPPPATH` automatically — no manual `build_flags` needed.

**Build-time guarantee:** the script runs on every `pio run`, so the version always reflects
the actual shas at build time even for incremental builds (not configure-time stale).

**CMake consumers:** `include("<breadboard>/cmake/bb_version.cmake")` before `project()` to
also set `PROJECT_VER` from the same logic (embeds into `esp_app_desc.version`).

**Fail-soft:** if git is unavailable, emits `dev-unknown` rather than erroring the build.

## Telemetry publisher (bb_mqtt, bb_pub, bb_sink_mqtt, satellites)

### TLS opt-in gates

All three sink-TLS gates default **n** — the default breadboard build uses plaintext sinks for the smallest footprint:

| Kconfig | Default | Notes |
|---------|---------|-------|
| `CONFIG_BB_MQTT_TLS_ENABLE` | n | TLS for the MQTT sink (`bb_mqtt`). |
| `CONFIG_BB_HTTP_TLS_ENABLE` | n | TLS for the HTTP telemetry sink (`bb_sink_http`). |
| `CONFIG_BB_TLS_MUTUAL_ENABLE` | n | Mutual TLS (client cert + key). `depends on BB_MQTT_TLS_ENABLE \|\| BB_HTTP_TLS_ENABLE`. |

Consumers opt in per-transport in their own `sdkconfig`. When a gate is OFF, the `bb_tls_creds` credential paths are skipped and the sink proceeds plaintext; no code changes required. The **update-check / OTA HTTPS path** (esp_http_client + crt-bundle) is **independent** of these sink gates — it always has TLS for the GitHub manifest/firmware fetch regardless of which sink gates are on. `BB_HTTP_CLIENT_TASK_STACK_SIZE` stays 8192 for that reason.

**`/api/info` capabilities.** `bb_tls_info` (auto-registers at PRE_HTTP tier via `CONFIG_BB_TLS_INFO_AUTOREGISTER`, default y) injects per-compiled-gate flags into `/api/info` `capabilities[]`: `"mqtt_tls"` when `BB_MQTT_TLS_ENABLE=y`, `"http_tls"` when `BB_HTTP_TLS_ENABLE=y`, `"mutual_tls"` when `BB_TLS_MUTUAL_ENABLE=y`. Clients interrogate this array to detect what TLS support was compiled in without a separate endpoint.

**`MBEDTLS_HARDWARE_SHA=n` requirement.** Apps that run a TLS sink alongside a miner driving the HW SHA engine via MMIO (e.g. TaipanMiner's stratum loop) must set `CONFIG_MBEDTLS_HARDWARE_SHA=n` in their sdkconfig. The HW SHA peripheral is non-reentrant; mbedTLS mid-handshake and the miner MMIO path share it and can corrupt each other's state. SW SHA decouples them at the cost of a slower TLS handshake (~200 ms extra on ESP32 at 240 MHz).

**Mode-aware `BB_PUB_WORKER_STACK`.** The worker stack auto-sizes: 4096 bytes when both TLS gates are OFF (plaintext only), 8192 bytes when `BB_MQTT_TLS_ENABLE` or `BB_HTTP_TLS_ENABLE` is ON. For the upstream esp-idf mbedTLS buffers, see the profile fragments in `sdkconfig/profiles/`.

**`bb_mqtt`** — portable MQTT client HAL. NVS namespace `bb_mqtt`; `/api/mqtt` PATCH/GET routes; TLS via `bb_tls_creds`. `bb_mqtt_publish(h, topic, payload, len, qos, retain)`. Telemetry config is **declarative / reboot-to-apply**: PATCH validates and persists to NVS only; exactly one enabled sink is wired at boot (registration == enabled); a reboot is required to apply changes (`POST /api/reboot`). PATCH returns `{"reboot_required":true}` on success; `GET /api/telemetry` exposes a top-level `pending_reboot` flag.

**`bb_pub`** — transport-agnostic telemetry core. Maintains a source registry and a fan-out set of `bb_pub_sink_t` sinks. `bb_pub_register_source(subtopic, sample_fn, ctx)` adds a source; `bb_pub_tick_once()` calls each source, injects shared `uptime_ms` (u64 ms via `bb_clock_now_ms64`), serializes the JSON once, then delivers it to every registered sink as `<prefix>/<hostname>/<subtopic>`. `bb_pub_set_sink` (back-compat, single-sink) replaces all sinks; `bb_pub_add_sink` appends to the fan-out set; `bb_pub_clear_sinks` empties it. A sink returning non-BB_OK is logged but does not abort delivery to other sinks. Periodic worker task registered at PRE_HTTP tier (`CONFIG_BB_PUB_AUTOREGISTER`, default y). Source cap: `CONFIG_BB_PUB_MAX_SOURCES` (default 8). Sink cap: `CONFIG_BB_PUB_MAX_SINKS` (default 4, range 1–8). Testing: `BB_PUB_TESTING` enables `bb_pub_test_reset()`. **Pause/resume:** `bb_pub_pause()` / `bb_pub_resume()` / `bb_pub_is_paused()` allow a consumer (e.g. an OTA pause hook) to quiesce publishing — while paused, `bb_pub_tick_once` is a cheap no-op (no sample_fn calls, no sink calls, no sockets/CPU). `bb_pub_pause()` uses a bounded-wait: it sets the paused flag (stopping new ticks) then waits on an in-publish condvar for any in-flight sink fan-out to complete, with a timeout of `CONFIG_BB_MQTT_NETWORK_TIMEOUT_MS` (default 30 s). This means pause() returns as soon as the in-flight publish finishes — not after the full TLS-write duration — preserving the no-concurrent-TLS guarantee without blocking OTA/update-check hooks for 14 s on slow mining boards. The tick lock (`s_tick_lock`) is released before the blocking `sk->publish()` / `buffer_replay` calls; only the CPU-bound sample + serialize phase runs under the lock. **Runtime config (NVS `bb_pub`):** `bb_pub_set_interval_ms(ms)` / `bb_pub_get_interval_ms()` — persist and live-apply the publish period (1 000–3 600 000 ms; ESP-IDF re-arms the timer immediately via `bb_pub_set_interval_apply_hook`). `bb_pub_set_enabled(bool)` / `bb_pub_is_enabled()` — persistent enable toggle (NVS key `enabled`, default on). Both gates are independent: tick publishes only when `enabled=true` AND not paused. The "publisher" `/api/telemetry` section is now read-write: GET reports `interval_ms` + `enabled`; PATCH sets and persists them with bounds validation. NVS re-arm requires on-hardware confirmation; host build updates in-RAM value only. **Payload extenders:** `bb_pub_register_payload_extender(fn, ctx)` registers a `bb_pub_payload_fn` callback invoked for each source's JSON object after `sample_fn` + `uptime_ms` injection but before serialize; extenders run in registration order and may add or alter any field (B1-270: source-in-body will be a built-in extender). Cap: `BB_PUB_MAX_PAYLOAD_EXTENDERS` (default 4, high-watermark warn at cap-1, `BB_ERR_NO_SPACE` on overflow). Not called when paused or disabled. Cleared by `bb_pub_test_reset()`.

**Store-and-forward ring buffer (`BB_PUB_BUFFER_ENABLE`).** When enabled, the ring has two modes selected by `BB_PUB_BUFFER_ALWAYS`:
- **On-failure mode (default, `BB_PUB_BUFFER_ALWAYS=n`):** ring is lazy-allocated (zero cost until an outage). When sink[0] fails, the serialized payload is captured. On the next tick where sink[0] succeeds, buffered entries are replayed oldest-first before live data. `captured_ms` is injected into any replayed entry that had a synced epoch at capture time (every buffered entry is by definition delayed, so injection is unconditional when `ts > 0`). Zero standing RAM cost.
- **Always-on mode (`BB_PUB_BUFFER_ALWAYS=y`):** every sink[0] publish is enqueued into the ring and drained each tick (enqueue → drain), providing a uniform ordered queue. Sinks[1..] keep direct fan-out unchanged. The ring is allocated eagerly at init (`bb_pub_buffer_init_eager()`), carrying a standing RAM cost of ~`MAX_ENTRIES × (TOPIC_MAX + 1 + MAX_PAYLOAD_BYTES)` bytes. Recommended only on PSRAM boards (allocation is SPIRAM-preferred). `captured_ms` is injected ONLY when an entry is genuinely delayed across ticks: age `(now_epoch_ms − capture_epoch_ms) > 1.5 × interval_ms`. Fresh entries drained the same tick they were enqueued are below the threshold and carry no `captured_ms` field, keeping healthy-operation points clean. `last_publish_ok` in always-on mode reflects whether the ring drained fully (empty after replay), not individual direct-publish results. **Oversized-entry fallback:** if an entry's `topic_len + 1 + payload_len` exceeds the ring slot (`BB_PUB_BUFFER_ENTRY_MAX`), `buffer_capture` returns `false` and the always-on path publishes the entry directly (live) instead of dropping it — it is not buffered for replay, but the live data is preserved. This is the correct behavior for topics like `info` whose serialized payload can exceed the ring slot on no-PSRAM boards.

**Exclusive-sink arbiter** — `bb_pub` enforces mutual exclusion between telemetry sinks: at most one exclusive sink may be active at a time. `bb_pub_exclusive_acquire(sink_id)` → `BB_OK` if slot is free or already held by `sink_id` (idempotent); `BB_ERR_CONFLICT` if held by a different id. `bb_pub_exclusive_release(sink_id)` frees the slot. Both `bb_mqtt_telemetry` and `bb_sink_http_telemetry` call acquire/release in their section `patch_fn` when `enabled` changes. A conflict returns `BB_ERR_CONFLICT` to `bb_telemetry_dispatch_patch`, which the route maps to **HTTP 409** with body `{"error":"another telemetry sink is active; disable it first"}`. **Boot precedence: first PRE_HTTP registrant wins.** Under the register-on-enable model, only the winning sink is ever registered as a `bb_pub` sink; the loser writes `enabled=0` to NVS. The MQTT loser stops its EARLY-connected auto-client via `bb_mqtt_stop_default()` (no `bb_mqtt_reconfigure` — that function no longer exists). The HTTP loser has no transport to tear down (session is lazy). In breadboard's default registration order `bb_mqtt_telemetry` registers before `bb_sink_http_telemetry` → MQTT wins by default. Consumer apps that register HTTP telemetry first get HTTP as the winner — precedence is registration-order-controlled, not hardwired. `BB_PUB_TESTING` exposes `bb_pub_exclusive_reset()` for test isolation.

**Publisher–sink coupling** — `PATCH /api/telemetry` couples the publisher's `enabled` flag to the sink enabled state. After per-section patches are applied, if any mqtt or http section in the body changed its `enabled` field, `bb_telemetry_dispatch_patch` reads the post-patch NVS state of both sinks and calls `bb_pub_set_enabled(any_sink_enabled)`. Semantics: enabling a sink → publisher enabled=true; disabling the last enabled sink → publisher enabled=false. Override: if the same PATCH body also explicitly sets `publisher.enabled`, that explicit value wins over the auto-coupling. This is implemented via the pure host-testable helper `bb_telemetry_couple_publisher(any_sink_enabled, publisher_explicit, publisher_explicit_value)` in `platform/host/bb_telemetry/bb_telemetry.c`; the coupling logic runs after `bb_section_dispatch_patch` returns. No coupling fires when the patch body changes only non-enabled fields of a sink section, or only touches the publisher section. Consistent with the reboot-to-apply model — the persisted flag is updated immediately, but takes effect at next boot.

**`bb_sink_mqtt`** — `bb_pub_sink_t` adapter that forwards payloads to `bb_mqtt_publish`. `bb_sink_mqtt(h, &out)` fills the sink struct.

**`bb_sink_http`** — `bb_pub_sink_t` adapter that publishes telemetry over HTTP via `bb_http_client_post`. Primary use-case: AWS IoT Core HTTPS publish (`https://<endpoint>:8443/topics/<encoded-topic>?qos=<n>`). NVS namespace `bb_sink_http` holds `base`, `path_tmpl` (default `/topics/{topic}?qos={qos}`), `qos` (default 1), `enabled`, `client_id` (sent as `X-Client-Id`; defaults to `bb_nv_config_hostname()` when empty), and `headers` (delimited string, up to `BB_SINK_HTTP_HEADERS_MAX`=8; format: one `[*]name: value` per `\n`-separated line, `*` prefix marks secret). TLS mutual-auth credentials resolved via `bb_tls_creds` from the same namespace. Generic `bb_http_client_session_set_header(session, key, value)` added to `bb_http_client` for per-header session configuration (host mock records applied headers for test assertions). GET/PATCH telemetry section emits structured `headers` array with per-row secret masking (`secret==true` → value omitted, `set:true` present; PATCH blank-value on secret row preserves stored value by name). Routes: GET/PATCH `/api/httppub` via `bb_sink_http_routes` (auto-registers at order 6; masks TLS creds, reports `ca_set`/`cert_set`/`key_set`). `bb_sink_http_url_encode` percent-encodes topic strings (slash → `%2F`). Pure host-testable fns: `bb_sink_http_parse_headers`, `bb_sink_http_serialize_headers`, `bb_sink_http_merge_headers`. Init: `bb_sink_http_init(cfg_or_null)` then `bb_sink_http(&sink)`. Sources: `components/bb_sink_http/` + `platform/host/bb_sink_http/bb_sink_http.c` (compiled host + espidf); `components/bb_sink_http_routes/` + `platform/espidf/bb_sink_http_routes/bb_sink_http_routes.c` + `platform/host/bb_sink_http_routes/bb_sink_http_routes_host.c`.

**Satellite sources** — each self-registers at PRE_HTTP tier (`CONFIG_BB_PUB_<X>_AUTO_ATTACH`, depends on `BB_PUB_AUTOREGISTER`). Hardware-specific satellites (`bb_pub_fan`, `bb_pub_power`, `bb_pub_thermal`) default **n** (opt-in per board); universal satellites (`bb_pub_info`, `bb_pub_wifi`) default **y**. Returns false (skip) when the HAL primary is absent.

| Component | Subtopic | Fields |
|-----------|----------|--------|
| `bb_pub_fan` | `fan` | `rpm`, `duty_pct`, `die_c`, `board_c` (number or null); when `BB_FAN_AUTOFAN`: `die_ema_c`, `vr_ema_c`, `pid_input_c`, `pid_input_src` |
| `bb_pub_power` | `power` | `vout_mv`, `iout_ma`, `pout_mw`, `vin_mv`, `temp_c` (number or null) |
| `bb_pub_thermal` | `thermal` | `soc_c`, `vr_c`, `asic_c`, `board_c` (number or null); skips only when all HAL primaries absent |
| `bb_pub_info` | `info` | `heap_internal_free`, `heap_internal_total`, `heap_internal_largest_block`, `heap_internal_min_free`, `psram_free`, `psram_total` (bytes), `rtc_used`, `rtc_total`, `flash_size`, `app_size`, `wdt_resets`, `uptime_ms`, `version` (string); always publishes |
| `bb_pub_wifi` | `wifi` | `rssi` (integer dBm); skips when STA not connected (`bb_wifi_has_ip()` false) |

Sources: `components/bb_pub_<x>/` (header, CMakeLists, Kconfig) + `platform/host/bb_pub_<x>/bb_pub_<x>.c` (compiled host + espidf).

### Telemetry memory tuning

Knobs that affect the bb_pub stack's RAM footprint (all in `sdkconfig` / `menuconfig`):

| Kconfig | Default | Notes |
|---------|---------|-------|
| `CONFIG_BB_PUB_WORKER_STACK` | 4096 (plain) / 8192 (TLS) | Worker task stack (bytes). Auto-sizes: 4096 when both `BB_MQTT_TLS_ENABLE` and `BB_HTTP_TLS_ENABLE` are OFF; 8192 when either is ON. Override in sdkconfig if needed. |
| `CONFIG_BB_PUB_MAX_SOURCES` | 8 | Source registry capacity. Each entry is a small struct (~24 B); rarely needs raising. |
| `CONFIG_BB_PUB_MAX_SINKS` | 4 | Sink array capacity (fan-out). Each entry is a function pointer + context pointer. |
| `CONFIG_BB_PUB_BUFFER_ENABLE` | n | Enable store-and-forward ring. Compiles in `bb_ring` dependency and capture/replay logic. Required for both on-failure and always-on modes. |
| `CONFIG_BB_PUB_BUFFER_MAX_ENTRIES` | 16 | Ring capacity (entries). Full ring evicts oldest entry, increments `dropped` counter. |
| `CONFIG_BB_PUB_BUFFER_MAX_PAYLOAD_BYTES` | 256 | Max bytes per ring entry payload. Oversized payloads are rejected (not truncated). |
| `CONFIG_BB_PUB_BUFFER_ALWAYS` | n | Always-on ring mode (`depends on BB_PUB_BUFFER_ENABLE`). When y: all sink[0] traffic routes through the ring (enqueue→drain each tick) for a uniform ordered queue. Carries standing RAM cost of ~`MAX_ENTRIES × (TOPIC_MAX + 1 + MAX_PAYLOAD_BYTES)`; ring is allocated eagerly at boot (SPIRAM-preferred). Recommended only on PSRAM boards. When n (default): ring is lazy and used only on sink failure, with zero standing cost. `captured_ms` is injected only when an entry is delayed beyond 1.5 × interval_ms in always-on mode; unconditionally (when `ts > 0`) in on-failure mode. |
| `CONFIG_BB_HTTP_CLIENT_TASK_STACK_SIZE` | 8192 | Referenced by `BB_HTTP_CLIENT_TASK_STACK` macro. Any task (including `bb_pub` worker) calling `bb_http_client_post` needs its stack ≥ this value. |
| `CONFIG_MQTT_TASK_STACK_SIZE` | 6144 | esp-mqtt internal task stack. Raise to 8192 when using TLS (MQTTS). |
| `CONFIG_MQTT_BUFFER_SIZE` | 1024 | esp-mqtt send/receive buffer (bytes). |
| `CONFIG_MQTT_OUTBOX_SIZE_BYTE` | 4096 | esp-mqtt QoS outbox (bytes, heap). |
| `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` | 16384 | TLS record input buffer. Drop to 4096 on no-PSRAM boards using HTTPS (AWS IoT accepts 4 KB records). |
| `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN` | 4096 | TLS record output buffer. |
| `CONFIG_MBEDTLS_DYNAMIC_BUFFER` | n | Dynamically allocate TLS buffers (frees them between records). Saves ~20 KB peak heap on no-PSRAM boards at the cost of more frequent allocs. |

**WROOM-32-class (no-PSRAM) guidance.** The classic ESP32 WROOM-32 has ~300 KB usable internal RAM. For plaintext-only deployments, keep `CONFIG_BB_PUB_WORKER_STACK=4096` and skip mbedTLS entirely. If TLS is required, set `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=4096`, enable `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`, and keep `CONFIG_BB_PUB_WORKER_STACK=8192`. The exclusive-sink arbiter guarantees only one telemetry sink (MQTTS or HTTPS) is ever active, so two concurrent TLS sinks cannot coexist by design. The remaining concurrent-TLS scenario to budget heap for is the active telemetry sink (MQTTS or HTTPS) running alongside the HTTPS update-check / OTA-pull client.

### Prometheus metrics (`GET /api/telemetry/metrics`)

Part of **`bb_telemetry`** (B1-295) — not a separate component. It shares `bb_telemetry`'s exact dependency closure (`bb_pub`, `bb_http`, `bb_json`, `bb_nv_config`), so per decision #402 ("extend by default; new component only when it isolates a distinct dependency closure") the route lives in `platform/espidf/bb_telemetry/bb_telemetry_routes.c` and is registered inside `bb_telemetry_init` alongside `/api/telemetry`. It enumerates all registered `bb_pub` sources, calls each sample function, and emits results in Prometheus exposition format or JSON.

**Query params:**

| Param | Values | Effect |
|-------|--------|--------|
| `format` | `prom` (default), `json` | Response format |
| `schema` | bare key (no value) | Return contract/metadata instead of live values |

**Four combos:**
- `GET /api/telemetry/metrics` → Prometheus text (`text/plain; version=0.0.4`) with live values
- `GET /api/telemetry/metrics?format=json` → JSON snapshot `{"host":..,"uptime_ms":..,"sources":{..},"publisher":{..}}`
- `GET /api/telemetry/metrics?schema` → `# TYPE` / `# HELP` lines only (no values)
- `GET /api/telemetry/metrics?schema&format=json` → `{"prefix":..,"metrics":[{name,type,source}],"publisher":[..]}`

**Prefix config:** the metric-name prefix is owned by `bb_pub`. Kconfig `CONFIG_BB_METRICS_PREFIX` (default `"bb"`, in `components/bb_pub/Kconfig`). Override at runtime: `bb_pub_set_metrics_prefix("taipanminer")` before init; the handler reads it via `bb_pub_metrics_prefix()`. All metric names are `<prefix>_<subtopic>_<field>` with non-alphanumeric chars sanitized to `_`. String fields are folded into a single `<prefix>_<subtopic>_info{..} 1` label-set metric.

**Publisher health gauges:** Always emitted: `<prefix>_pub_source_count`, `<prefix>_pub_buffer_count`, `<prefix>_pub_buffer_dropped`, `<prefix>_pub_ring_undersized`, `<prefix>_pub_last_publish_age_ms`.

**Auto-register:** registered by `bb_telemetry_init` at order 5 via `bb_registry` (`CONFIG_BB_TELEMETRY_AUTOREGISTER`, default y), alongside the `/api/telemetry` GET/PATCH routes.

## Releases

Tagging is manual: `git tag -a vX.Y.Z -m 'chore: vX.Y.Z tag' && git push origin vX.Y.Z`. The `release.yml` workflow waits for CI then publishes a GitHub release with auto-generated notes categorized by PR label (`.github/release.yml`). PR labels are auto-applied from conventional-commit prefixes; `new-component` PRs need that label set manually.

## Workspace conventions

Workspace-level conventions (git, testing, docs) live in `/Users/jae/Projects/dangernoodle/CLAUDE.md`.

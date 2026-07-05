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
- **Cross-cutting types live in `bb_core`.** `bb_err_t`, `BB_OK`/`BB_ERR_*` macros, `bb_http_handle_t`, `bb_http_request_t`, `bb_http_pause_cb_t`, and `bb_http_resume_cb_t` are defined in `components/bb_core/include/bb_core.h`. REQUIRES `bb_core` for these types — never `bb_nv` for the error type or `bb_http` for the handle typedef alone. `bb_ota_pause/resume_cb_t` and `bb_ota_check_pause/resume_cb_t` are aliases for the `bb_core.h` typedefs. Portable `bb_clock_now_ms()` lives in `bb_core/include/bb_clock.h`.
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
- **Route/SSE-attach functions are pure httpd** — no timer/task creation inside a `BB_INIT_REGISTER[_N]` (REGULAR-tier) init.
- **Background work self-registers at PRE_HTTP** via a dedicated `<component>_start()` doing state-init + timer-create + timer-arm only — mirror `bb_pub_start()` (`platform/espidf/bb_pub/bb_pub_espidf.c`).
- Never call `bb_timer_*_create` from a `BB_INIT_REGISTER`-registered fn — this shipped 3× (bb_net_health, bb_health_stack, bb_update_check).

**Periodic idiom-duplication sweep.** A diff-scoped review misses cross-file duplication (it's how the SPIRAM/status/query/section/HAL dup shipped). Before cutting a release, run the whole-repo idiom sweep (workflow `idiom-sweep` / `.claude/workflows/idiom-sweep.md`): parallel finders over allocation, concurrency, http-serialization, config/peripheral, and clock/timestamp idioms, each reporting ≥2-site repeats with a proposed helper. The header/type-leak CI lint (B1-263) is the complementary enforcement for REQUIRES/portability hygiene.

## Logging

Use `bb_log_{e,w,i,d,v}(tag, fmt, ...)` macros for all breadboard component code. On ESP-IDF these expand to `ESP_LOG{E,W,I,D,V}`; on host they map to `fprintf` (debug/verbose compile out to keep test output clean).

The `bb_log_stream` ringbuffer is lazy-allocated on the first call to `bb_log_stream_init()` (heap-allocated, SPIRAM-preferred with fallback to default heap), reducing BSS footprint by ~6 KB when no log consumer is attached. The allocation is idempotent; subsequent calls return `BB_OK` immediately. Buffer size is tunable via `CONFIG_BB_LOG_STREAM_BUF_BYTES` (default 6144 bytes).

**Structured log stream (`bb_log_event`).** `CONFIG_BB_LOG_EVENT_AUTO_ATTACH` (default y) registers a non-retained `"log"` bb_event topic and forwards each log line as `{"ts":<u64 ms>,"level":"I|W|E|D|V|?","tag":"...","msg":"..."}` at `GET /api/events?topic=log`. This is the primary log transport; the legacy `/api/logs` raw-text SSE route has been retired. Forwarder queue depth is tunable via `CONFIG_BB_LOG_EVENT_QUEUE_LEN` (default 48 (SPIRAM) / 24 (no-PSRAM); ~9 KB / ~4.6 KB at 192 B/line). Drop count observable via `bb_log_event_dropped()`. The `bb_log_stream` ringbuf and `bb_sink_ws` drain path are preserved for WebSocket consumers.

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
This should be empty (outside the LVGL exception). The CI `check` step (`make check` = `lint` + `cppcheck`) enforces this automatically via `python3 scripts/bbtool.py lint --root . --profile library` (B1-263). The same lint also runs as part of every firmware build (see `bb_lint()` / `BB_LINT_ON_BUILD` below) — zero rule divergence between the two paths, so `make all` (static analysis + py-tests + host tests) drops the standalone lint step and relies on the build-time gate instead; run `make lint` to invoke it standalone. Run `make all` locally before pushing — it excludes `smoke`, which CI also gates on but is too slow for a local pre-push loop.

### Tests must not reach into other components

- Tests may `#include` any public header (`components/<name>/include/<name>.h`).
- Tests MUST NOT `#include` private implementation headers. If a test needs a hook, the component exposes it via `<name>_test.h` guarded by `BB_<NAME>_TESTING`.

### Includes at file top

`#include` lives at the top of the file. Mid-file `#include` directives hide dependencies; conditional gating belongs around the *use* of the symbol, not the include itself.

## Embedding assets

Assets are embedded via `bb_embed_assets()` / `bb_embed_site()` cmake helpers in
`cmake/bbtool.cmake`; consumers include it before `project()`. See
`scripts/bbtool/README.md` for full usage, wiring, and the `flash_factory.py`
post-script. `bb_lint()` (same file) is wired into every build by default
(`BB_LINT_ON_BUILD=ON`) — it runs `scripts/bbtool.py lint --root . --profile
library` (the same rules as `make check`) and fails the build on any lint
error. Set `BB_LINT_ON_BUILD=OFF` to disable it as a deliberate override.

## Registry (handler-lifecycle)

`bb_init` holds three ordered lists — EARLY → PRE_HTTP → REGULAR — driven by two app calls: `bb_init_init_early()` then `bb_init_init()`. `bb_init_init()` walks PRE_HTTP, optionally autostarts the HTTP server (`CONFIG_BB_HTTP_AUTOSTART`, default y), then walks the regular tier. Components self-register at link time via a macro; disabling the corresponding `CONFIG_BB_<NAME>_AUTOREGISTER` Kconfig (default y) removes the registration without touching the public API.

**EARLY tier** — NVS, WiFi init, board bring-up (no server arg):
```c
BB_INIT_REGISTER_EARLY(bb_<comp>, bb_<comp>_init);          // order=0 (default)
BB_INIT_REGISTER_EARLY_N(bb_<comp>, bb_<comp>_init, N);     // explicit order N
```

**PRE_HTTP tier** — tasks that must run after EARLY but before the HTTP server (no server arg):
```c
BB_INIT_REGISTER_PRE_HTTP(bb_<comp>, bb_<comp>_init);       // order=0 (default)
BB_INIT_REGISTER_PRE_HTTP_N(bb_<comp>, bb_<comp>_init, N);  // explicit order N
```

Like the regular tier, `BB_INIT_REGISTER_EARLY_N` / `BB_INIT_REGISTER_PRE_HTTP_N` accept an integer `order` as the third argument — each tier's walker sorts its own entries by `.order` ascending before invoking inits, ties preserve registration (insertion) order. EARLY and PRE_HTTP are **not** unordered: they're independently ordered lists, same sort semantics as REGULAR, just no server arg and a separate namespace of order values (an EARLY order N has no relationship to a PRE_HTTP or REGULAR order N). Example: `BB_INIT_REGISTER_PRE_HTTP_N(bb_<comp>, bb_<comp>_init, -1);` runs before the default order-0 PRE_HTTP entries — use a negative order to guarantee a component initializes before every plain `BB_INIT_REGISTER_PRE_HTTP` registrant.

**Regular tier** — HTTP route registration (server arg):
```c
BB_INIT_REGISTER(bb_<comp>, bb_<comp>_init);        // order=0 (default)
BB_INIT_REGISTER_N(bb_<comp>, bb_<comp>_init, N);   // explicit order N
```
`BB_INIT_REGISTER_N` accepts an integer `order` as the third argument. The regular-tier walker sorts entries by `.order` ascending before invoking inits — lower numbers run first. Ties preserve registration (insertion) order, i.e. the sort is stable. `BB_INIT_REGISTER` is shorthand for `BB_INIT_REGISTER_N(..., 0)`. **N is a sort key only — not a route-count hint.** Example: `bb_event_routes` registers at order 0; `bb_ota_check` registers at order 4 so `bb_event_routes_init` (which sets `s_cfg.initialized = true`) has already run before `bb_ota_check` calls `bb_event_routes_attach`. `bb_diag_routes` also registers at order 4 (was implicit order 0) — same rationale: its optional `CONFIG_BB_DIAG_AUTO_ATTACH` diag.boot SSE auto-attach calls `bb_event_routes_attach_ex`, which needs `bb_event_routes_init` to have already run.

All three tiers require the matching CMake helper (in `cmake/bb_init.cmake`) to prevent garbage-collection under PlatformIO:

```cmake
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_init.cmake")
bb_init_force_register(${COMPONENT_LIB} bb_<name>)           # regular
bb_init_force_register_early(${COMPONENT_LIB} bb_<name>)     # early
bb_init_force_register_pre_http(${COMPONENT_LIB} bb_<name>)  # pre_http
```

Today this pattern owns — regular: `bb_ota_pull`, `bb_ota_push`, `bb_info`, `bb_log` (routes), `bb_log_event` (log stream topic), `bb_manifest`, `bb_ota_validator`, `bb_wifi` (routes), `bb_system` (routes), `bb_openapi`, `bb_mdns`, `bb_event_routes`, `bb_ota_check`, `bb_diag` (routes). Early: `bb_log_stream`, `bb_nv_flash`, `bb_nv_config`, `bb_wifi` (STA init via `CONFIG_BB_WIFI_AUTOREGISTER`), `bb_diag_panic`, `bb_event`. PRE_HTTP: `bb_http_reserve_routes(N)` is vestigial (see Route-sizing model below) but companion init functions may still call it harmlessly. HTTP server autostart is gated on `CONFIG_BB_HTTP_AUTOSTART` (default y); disable it if CORS or OpenAPI config must precede server start. Socket reservation for non-httpd usage (stratum TCP, mDNS UDP, transient outbound) is tunable via `CONFIG_BB_HTTP_LWIP_RESERVE` (default 3, range 1–6) — lower it to give httpd more headroom, raise it to preserve slots for outbound work.

**Event loop noise suppression (B1-306).** `bb_http` registers a no-op handler for `ESP_HTTP_SERVER_EVENT` on the default event loop at server start to suppress esp_event "no handlers" DEBUG spam that self-amplifies when an SSE viewer (`/api/events`) is open.

**Route-sizing model.** All `/api/*` routes are served by per-method wildcard httpd handlers (`GET/POST/PUT/PATCH/DELETE /api/*`, registered at server start before any consumer asset `GET /*`) that dispatch internally via the `bb_dispatch_api` table (`BB_DISPATCH_API_CAP`, default 64). A high-watermark `bb_log_w` fires once when count reaches `CAP-8`; overflow returns `BB_ERR_NO_SPACE` (non-fatal, logged). `/api/*` routes do **not** consume httpd handler slots. `max_uri_handlers` (`BB_HTTP_MAX_URI_HANDLERS`, default 12, constant) covers only the fixed wildcards (5 api + OPTIONS + GET asset) plus headroom for non-`/api` routes (`/save`, captive `/*`). `bb_http_reserve_routes()` is vestigial (ABI kept; body is a no-op on espidf; calls from existing PRE_HTTP companions are harmless). When adding a new `/api/` endpoint, no PRE_HTTP companion or reserve call is needed — just call `bb_http_register_route` in the regular tier. When adding a non-`/api` httpd route (e.g. a captive-portal path), raise `BB_HTTP_MAX_URI_HANDLERS` in your `sdkconfig` if headroom is exhausted.

`bb_http_register_route` and related registration functions return `BB_ERR_NO_SPACE` when the route registry is full. Registry capacity is tunable via `CONFIG_BB_HTTP_ROUTE_REGISTRY_CAP` (default 64). After the regular-tier walk, `bb_init_init()` audits the registry count and panics if overflow occurred (`CONFIG_BB_HTTP_ROUTE_REGISTRY_STRICT`, default y) or emits a high-watermark warning at count >= CAP-8.

Duplicate (method, path) registrations in the `/api/*` dispatch table are detected at `bb_dispatch_api_add` time: the second registration is warned (`bb_log_w`) and dropped (first-wins); callers receive `BB_ERR_INVALID_STATE`. Enable `CONFIG_BB_HTTP_ROUTE_DUP_STRICT` (default n) to escalate a duplicate to a hard `assert` — useful in CI/smoke builds to catch accidental route collisions early.

**`bb_dispatch_cmd` — transport-neutral command registry (B1-543, WS command-plane epic B1-542).** The `bb_dispatch_` family's second member: where `bb_dispatch_api` keys on `(method, path)` and hands out an httpd-flavoured `bb_http_handler_fn`, `bb_dispatch_cmd` keys on a plain **action string** and hands out a `bb_json_t` in/out handler (`bb_dispatch_cmd_handler_t`) with no HTTP concept at all — the SSOT for dispatching a named command regardless of transport. `bb_dispatch_cmd_register(action, handler, ctx)` returns `BB_ERR_NO_SPACE` at capacity (`CONFIG_BB_DISPATCH_CMD_CAP`, default 32) or `BB_ERR_INVALID_STATE` on a duplicate action (first-wins, `bb_log_w`, same policy as `bb_dispatch_api`). An optional single authorization checkpoint (`bb_dispatch_cmd_set_authorizer`, NULL = allow all) is consulted by `bb_dispatch_cmd_call` before every handler invocation. Component: `components/bb_dispatch_cmd/` (public header, `bb_core`+`bb_json` only — no ESP-IDF types); portable impl `platform/host/bb_dispatch_cmd/bb_dispatch_cmd.c` (compiled host + espidf, pure C). **Ships tested-but-unused** — exactly as `bb_dispatch_api` did initially: no transport wires it up yet. Consumers (`bb_sub_http`/`bb_sub_ws`, planned) will layer envelope parsing on top; extracting an `action`/`args` pair from a raw WS/HTTP payload and calling `bb_dispatch_cmd_call` is a separate later component (`bb_dispatch_envelope`, B1-563), not part of this registry.

**Display self-registration.** `BB_DISPLAY_AUTOREGISTER(chip, kconfig_sym, backend_ptr)` macro (in `bb_display_spi_common`) replaces per-driver constructor boilerplate. SSD1306 framebuffer is heap-allocated at init; `bb_display_ssd1306_init` can return `BB_ERR_NO_MEM`. ili9341 and st77xx share a bounce buffer via `bb_display_spi_common` for blit and clear operations.

**Four registration mechanisms exist, each for a distinct shape — not overlapping choices:** (1) `bb_init` tier registration (`BB_INIT_REGISTER_*`, above) for component lifecycle/route init; (2) `bb_registry`'s generic name→`void*` table (`bb_task_registry`/`bb_ring_registry`/`bb_event_topic_registry`) for named lookup of live handles; (3) `bb_http_register_described_route` (below, Provisioning UI / OTA strategy) as the OpenAPI route SSOT; (4) `bb_display`'s own `s_backends[]` fixed array + `bb_display_register_backend()` (`bb_display.c`), self-populated via `BB_DISPLAY_AUTOREGISTER`'s `__attribute__((constructor))` + a per-backend `-u bb_display_register__<chip>` linker force-include (see each `bb_display_<chip>/CMakeLists.txt`). **(4) is intentional, not a gap to fold into (2):** it implements ordered probe-and-select-first-success (`bb_display_init` walks `s_backends[]` in registration order, calling each backend's optional `probe()` then `init()`, keeping the first that succeeds) — a semantic `bb_registry`'s flat name→ptr lookup table does not have. Folding it into `bb_registry` would save the handful of lines the dedicated `s_backends[]` array costs, at the cost of the compile-time capacity enforcement pattern `bb_registry` consumers rely on (`_Static_assert(cap <= BB_REGISTRY_SNAPSHOT_MAX)`) not carrying over cleanly to an ordered-probe walk — not a net win. Keep them separate. `bb_registry` also supports an opt-in pointer-identity-keyed mode (`register_ptr`/`deregister_ptr`/`lookup_ptr`/`foreach_ptr`, comparing the key via `==` instead of `strcmp`), used by `bb_task`'s TaskHandle_t-keyed base task registry (task-registry unification, see below).

**Unified task registry (task-registry unification).** `bb_task` owns a single pointer-keyed base registry of live tasks (`bb_task_base_upsert`/`bb_task_base_touch`/`bb_task_base_sweep_apply`/`bb_task_base_remove`), populated two ways: (1) `bb_task_create()` upserts on creation; (2) `bb_task_registry`'s periodic base-scan job (`bb_task_registry_base_scan_apply`, cadence `CONFIG_BB_TASK_REGISTRY_BASE_POLL_MS`, ESP-IDF shell reads `uxTaskGetSystemState`) upserts/touches every live task each pass and sweeps entries that have disappeared. `bb_task_registry_register()`/`_deregister()` join/unjoin this base registry by handle, layering the overlay's authoritative budget/watchdog-arm state on top — the lock-free `bb_task_registry_feed()` hot path is unchanged. `bb_health_stack` is now a pure observer: it no longer keeps its own task table or polls via `bb_timer` (`CONFIG_BB_HEALTH_STACK_MAX_TASKS` retired, sharing `CONFIG_BB_TASK_BASE_MAX` instead); it registers a low-stack callback via `bb_task_registry_set_low_stack_handler()` and reacts to transitions the base-scan detects. This also broke the `bb_timer`↔`bb_task_registry` dependency cycle: `bb_timer`'s task-creation sites now use `bb_task_create()` directly and self-manage their own watchdog feed, so `bb_timer` no longer requires `bb_task_registry`.

The `bb_nv_config` component persists device configuration to NVS: WiFi credentials, hostname (32 chars max, RFC1123 charset), and DHCP/mDNS feature flags. Hostname is available via `bb_nv_config_hostname()` and settable via `bb_nv_config_set_hostname()` with validation for leading/trailing hyphens and non-alphanumeric characters. RTC slow-memory creds backup (`CONFIG_BB_NV_CREDS_RTC_BACKUP`, default y): mirrors ssid/pass/provisioned into `RTC_NOINIT_ATTR` so a crash-induced NVS erase doesn't strand a headless board; restored and healed on next boot (`bb_nv_config_creds_restored()`); erase observability via `bb_nv_config_was_erased()` (both always defined under ESP_PLATFORM).

`bb_wifi_autoinit` (EARLY tier) reads the hostname from `bb_nv_config_hostname()` and applies it before connecting so DHCP/mDNS see the right name on first packet; on validated builds it retries the STA connect indefinitely (30 s backoff, `CONFIG_BB_WIFI_RETRY_FOREVER_WHEN_VALIDATED`, default y) so a network outage doesn't knock the device into AP mode; errors are swallowed so the EARLY walker continues for provisioning flows.

`bb_wifi` routes (regular tier, `platform/espidf/bb_wifi/bb_wifi_routes.c`): `GET /api/wifi` (connection info), `POST /api/scan` (scan trigger), and — when `CONFIG_BB_WIFI_RECONFIGURE=y` (default) — `PATCH /api/wifi {ssid, password}` → 202 + deferred reboot via `bb_wifi_reconfigure` (brick-safe; reserve bumps from 2 to 3).

The `wifi_reconn` FSM (`platform/espidf/bb_wifi/wifi_reconn.c`) drives reconnect. ST_CONNECTING is bounded by `WIFI_RECONN_CONNECTING_TIMEOUT_MS` (30 s): if neither GOT_IP nor DISCONNECT arrives, the task calls `wifi_reconn_policy_on_connect_timeout`, re-issues `esp_wifi_disconnect` + `esp_wifi_connect`, and stays in ST_CONNECTING, escalating to the safeguard reboot (`do_safeguard_reboot`) after the 5-min persistent-fail window — same path used by disconnect-triggered reboots.

**Lost-IP recovery (B1-365).** When `IP_EVENT_STA_LOST_IP` fires while the reconnect manager is active, `bb_wifi.c` calls `wifi_reconn_on_lost_ip()` which bumps `s_state.lost_ip_count`, `last_lost_ip_us`, and `reason_histogram[WIFI_REASON_BB_LOST_IP]` (sentinel 99, reserved breadboard value — esp_wifi reasons use 1-24/53-67/200-208; 99 is free and < 256) via `wifi_reconn_policy_on_lost_ip`, then issues `esp_wifi_disconnect()` (without setting `s_self_disconnect`) so the resulting `WIFI_EVENT_STA_DISCONNECTED` flows into `wifi_reconn_on_disconnect()` and drives full recovery. A second trigger: `reconn_task` in `ST_IDLE` polls every `CONFIG_BB_WIFI_NO_IP_WATCHDOG_S` seconds (default 60 s, bridged as `WIFI_RECONN_NO_IP_WATCHDOG_MS`) via `esp_wifi_sta_get_ap_info` + `bb_wifi_has_ip()` — boot-safe since `esp_wifi_sta_get_ap_info` returns non-`ESP_OK` when not associated. Recovery is observable via `bb_wifi_get_lost_ip_count()` / `bb_wifi_get_lost_ip_age_s()` (public API) and as `lost_ip_recoveries` / `lost_ip_age_s` in the `net.health` SSE topic and `/api/health` net section.

**Inactive-time zombie recovery (B1-369).** `CONFIG_BB_WIFI_NO_IP_WATCHDOG_ENABLE` (default n) gates both mode-a and mode-b recovery. When disabled, `ST_IDLE` uses `portMAX_DELAY` (no periodic watchdog). When enabled: (mode-a) ST_IDLE watchdog fires every `CONFIG_BB_WIFI_NO_IP_WATCHDOG_S` seconds; (mode-b) `esp_wifi_set_inactive_time(WIFI_IF_STA, CONFIG_BB_WIFI_INACTIVE_TIME_S)` (default 45 s, minimum 3) is called after `esp_wifi_start()` so the driver emits `WIFI_EVENT_STA_DISCONNECTED` after beacon starvation, flowing into the normal reconnect FSM path — the safeguard-reboot chain arms automatically via the existing `wifi_reconn_on_disconnect` → `wifi_reconn_policy_on_disconnect` → `first_fail_us` → 5-min window path. `bb_wifi_restart_sta()` performs a full stop/start cycle instead of a bare `esp_wifi_disconnect()`: it sets `s_sta_restarting` (suppresses the auto-connect fired by `WIFI_EVENT_STA_START`), calls `wifi_reconn_absorb_next_disconnect()` (absorbs the synthetic `WIFI_EVENT_STA_DISCONNECTED` from `esp_wifi_stop()`), restarts the driver, re-applies the saved `s_sta_config` and inactive-time, then calls `esp_wifi_connect()` explicitly. Both FSM reconnect-action sites (immediate retry + backoff elapsed) and `bb_wifi_force_reassociate()` call `bb_wifi_restart_sta()` when enabled; fall back to `esp_wifi_connect()` / `esp_wifi_disconnect()` when disabled.

**App-driven recovery request (B1-371, superseded the probe-tick design after PR #578).** The original design polled an egress probe on an `ST_IDLE` tick with `CONFIG_BB_WIFI_EGRESS_PROBE_S`/`CONFIG_BB_WIFI_EGRESS_PROBE_FAILS` Kconfig knobs; those knobs and the tick-driven probe are gone. Recovery is now app-driven: any caller (e.g. an MQTT publish failure, an HTTP client timeout) calls `bb_wifi_request_recovery(const char *reason)` (`bb_wifi.h`, always compiled). If the STA has no IP yet, it is a no-op (`BB_OK`) — the FSM already owns recovery via the no-IP watchdog / disconnect path. Otherwise it is debounced by `CONFIG_BB_WIFI_RECOVERY_COOLDOWN_S` (default 60 s, range 10–3600): calls inside the cooldown window are logged and dropped (never silent). On a non-debounced call it invokes `wifi_reconn_request_recovery(reason)`, which enqueues `EVT_RECOVERY_REQUEST` (with the reason string) onto the reconn task's queue. The reconn task's `EVT_RECOVERY_REQUEST` handler calls `bb_wifi_restart_sta()`, arms `first_fail_us` for the persistent-fail safeguard window, bumps `egress_dead_count`, and increments `reason_histogram[WIFI_REASON_BB_EGRESS_DEAD]` (sentinel 100, < 256, reserved). This covers b2-class egress zombies (board has IP + L2 association but no real egress to DNS/MQTT) without a dedicated polling probe — the app already knows when egress has failed. **Ping API** (unchanged public surface, corrected signature): `bb_wifi_ping(uint32_t target_addr, uint32_t timeout_ms, bool *out_reachable)` — `target_addr` is a raw IPv4 address in `esp_ip4_addr_t.addr` byte order; a lazy-session `esp_ping` (one ICMP echo-request, caller-supplied timeout) sets `*out_reachable`; session created lazily and reused (one binary semaphore for signaling; mutex guards session create/reuse; ALWAYS compiled so CI validates the symbol). `bb_wifi_gateway_reachable(uint32_t timeout_ms)` — convenience wrapper: gets the STA default gateway via `esp_netif_get_ip_info(s_sta_netif, &ip_info)` and pings `ip_info.gw.addr`, returns false if no IP/gateway info is available. Both declared inside `#ifdef ESP_PLATFORM` in `bb_wifi.h`. Host stub: `bb_wifi_get_egress_dead_count()` returns 0. `wifi_reconn_policy_on_egress_probe` (`wifi_reconn_policy.{c,h}`, tracks `s_state.egress_fail_streak`) now has a LIVE observe-only caller (see below) in addition to `test/test_host/test_wifi_reconn_policy.c` — it is no longer orphaned/test-only, but its returned action is still never acted on in production.

**Gateway-probe worker, observe-only (B1-518 PR2, Phase 1 of the egress-recovery SSOT).** `platform/espidf/bb_wifi/bb_wifi_gw_probe.c`, gated `CONFIG_BB_WIFI_GW_PROBE_ENABLE` (**opt-in, default n** — a periodic ping worker + dedicated task is not imposed on every consumer; our fleet enables it per-board in sdkconfig, plus `_PERIOD_S`/`_TIMEOUT_MS`/`_FAILS`/`_STACK`/`_PRIORITY`), runs `bb_wifi_gateway_reachable()` on a dedicated `bb_timer_worker` task (own task, not the shared `bb_timer_disp` task — the ping can block up to `timeout_ms + 500ms`). It feeds each probe result into `wifi_reconn_policy_on_egress_probe()` — a LIVE caller, but strictly observe-only — using a SEPARATE, worker-owned `wifi_reconn_state_t` (never the live FSM state owned by `wifi_reconn.c`). The result is recorded via `bb_wifi_gw_status_t` (retrievable via `bb_wifi_get_gateway_status()`); the classifier's returned `wifi_reconn_action_t` is used only to increment `gw_dead_count` (a would-have-tripped counter) and is otherwise DISCARDED — no `bb_wifi_restart_sta` / `bb_wifi_request_recovery` / `esp_wifi_*` call is ever made from this file. This is Phase 1 of the egress-recovery single-source-of-truth (B1-518); the recovery ACTION (wiring the discarded action into `bb_wifi_request_recovery`) is a later, default-off gate.

## Satellite / extender opt-in convention

A **satellite** is a small component that contributes a section to an existing endpoint (e.g. a named section on `/api/health` via `bb_health_register_section`, or on `/api/info` via `bb_info_register_section`) without owning the route itself. Three tiers exist:

**Tier 1 — In-component Kconfig gate** (`CONFIG_BB_<X>_ROUTES_AUTOREGISTER`, default n): the extender exposes the owning component's own data and lives inside that component. No separate component. Examples: `bb_power_routes`, `bb_fan_routes`, `bb_log_routes`.

**Tier 2 — Separate component + Kconfig auto-register** (`CONFIG_BB_<X>_AUTOREGISTER`, default n for health/info satellites; default y for infrastructure routes): the satellite lives in its own component directory, bridges two independent dep closures, and self-registers via `BB_INIT_REGISTER[_N]` in the ESP-IDF `.c` file, gated by `#if CONFIG_BB_<X>_AUTOREGISTER`. The CMakeLists conditionally calls `bb_init_force_register` under `if(CONFIG_BB_<X>_AUTOREGISTER)`. Examples after B1-347: `bb_temp`, `bb_mqtt_info`, `bb_led_info`. Pre-existing examples: `bb_net_health`, `bb_pub_*`.

**Tier 3 — Manual register() — DEPRECATED for new satellites.** The three legacy satellites (`bb_temp`, `bb_mqtt_info`, `bb_led_info`) keep their manual fns (`bb_temp_register_info`, `bb_mqtt_register_health`, `bb_led_register_info`) as escape hatches for consumers with ordering constraints. **Use auto XOR manual** — enabling `AUTOREGISTER` while also calling the manual fn double-registers the section (second registration is warned and dropped; first-wins per route registry policy). Flipping these to `default y` and removing the manual calls from TaipanMiner is a cross-repo follow-up, not part of this change.

**Naming split:** `CONFIG_BB_<NAME>_AUTOREGISTER` for core/infrastructure components and health/info satellites (this file); `CONFIG_BB_<NAME>_AUTO_ATTACH` for event-topic satellites that attach a `bb_event` topic to `bb_event_routes` (see Auto-attach convention below). `bb_display_info` uses `CONFIG_BB_DISPLAY_INFO_AUTO_ATTACH` — the one legacy exception to the `_AUTOREGISTER` name on a satellite; do **not** rename it.

## bb_cache — canonical state-key cache

`bb_cache` is the canonical state-key cache + shared serializer: one registered serializer per key guarantees SSE event payloads and REST handler payloads are byte-identical by construction. Two ownership modes: owned (`snapshot == NULL`, caller copies in via `bb_cache_update`) and getter (`snapshot != NULL`, bb_cache reads through the caller's accessor).

**`bb_cache_register(const bb_cache_config_t *cfg)` (B1-576, config-struct API — collapsed `bb_cache_register`/`_ex`).** `cfg->key` is registry identity — it is **copied** into a fixed `CONFIG_BB_CACHE_KEY_MAX`-byte buffer (default 96, sized to cover `bb_sub`'s dynamic ingress keys), so the caller does not need to keep the string alive past the call; `strlen(cfg->key) >= CONFIG_BB_CACHE_KEY_MAX` is rejected loudly with `BB_ERR_INVALID_ARG`, never silently truncated. **Zero-initializing `cfg->flags` means `BB_CACHE_FLAG_NONE`** — callers that need SSE fan-out MUST set `.flags = BB_CACHE_FLAG_SSE` explicitly (the retired positional `bb_cache_register` defaulted to SSE; the struct form does not).

**Topic vs key naming.** `key` is bb_cache's registry-identity field. `topic` is reserved for the wire/pub-sub name: the SSE event topic a key's `BB_CACHE_FLAG_SSE` binds to, `?topic=` on `GET /api/events`, and MQTT topics — these are often the *same string* as a key (e.g. `BB_OTA_CHECK_TOPIC "update.available"` names both the cache key and the SSE topic it's bound to), but the two concepts are named differently based on which role a given call site is playing.

**Keyed enumeration + compact read (additive, backward-compatible):**
- `bb_cache_count()` — number of currently registered keys.
- `bb_cache_key_at(index, &out_key)` — raw registry-slot lookup, `[0, BB_CACHE_MAX_TOPICS)`; `out_key` may come back NULL for a free slot.
- `bb_cache_foreach(cb, ctx)` — invokes `cb` once per registered key. The key set is snapshotted under the registry lock and released before invoking `cb`, so `cb` may safely call back into `bb_cache_*`.
- `bb_cache_get_raw(key, buf, cap)` — **owned-mode only**: copies `min(registered size, cap)` raw struct bytes into `buf`. Returns `BB_ERR_INVALID_STATE` for getter-mode keys (no owned struct to read).

These four are the enumerable/compact read path for consumers (e.g. the brood fleet store) that want to walk the registry by key and read a struct's raw bytes without going through JSON serialization.

**Convention: config-struct registration.** `bb_*_register` functions take a single config struct (`bb_*_config_t`) by pointer, not positional args — features extend by adding struct fields, never breaking signatures; retires the `foo`/`foo_ex` positional-expansion pattern (see `bb_cache_register` above).

**Convention: API variant naming.** A mutating/registering call with optional or growing parameters takes a **single params-struct as its sole argument** — never `_ex`/`_exN`, never a base+variant split. Genuinely-distinct *behavior* gets a capability name (`_into`, `_locked`, `_from`), not a suffix ladder. Instances: `bb_cache_register`, `bb_event_subscribe`, `bb_cache_update` (B1-600, collapsed `bb_cache_update`/`_ex` into `bb_cache_update(const bb_cache_update_t *req)`, which also added a `ts_ms` override field for ingress/self-emit sources that need to stamp a non-"now" sample time).

**Convention: bb optional features are opt-in.** Kconfig defaults to `n`; the consuming app enables what it needs. Never default an optional feature `y` "for the constrained board's sake" — the constrained board (e.g. ESP32-C3) is already safe by default because it simply doesn't opt in; boards with headroom flip it on explicitly in their own sdkconfig. Instance: `CONFIG_BB_CACHE_REACTIVE_ENABLE` (B1-589).

## bb_cache_reactive — change-driven observer layer on bb_cache (B1-589 PR-4b)

`bb_cache_reactive` is a Kconfig-gated (`CONFIG_BB_CACHE_REACTIVE_ENABLE`, default n — see the opt-in convention above) sibling component to `bb_cache` that adds change-driven push on top of `bb_cache`'s poll/get-serialized read path. When disabled, `bb_cache_reactive_observe()` is a static-inline no-op (`BB_ERR_UNSUPPORTED`) and `bb_cache_reactive_update()` is a static-inline passthrough to `bb_cache_update()` — callers may invoke either unconditionally with no `#ifdef`.

**Observer triad (config-struct, single arg, no `_ex`):** `bb_cache_reactive_observer_t { key, on_register, on_change, on_remove, ctx }` registered via `bb_cache_reactive_observe()`. `key == NULL` observes every key; a non-NULL key observes exactly that key. **PR-4b scope: only `on_change` fires.** `on_register`/`on_remove` are accepted and stored but never invoked — reserved for a later PR (`on_remove` needs delete/eviction, B1-592; `on_register` needs a new-key-notify hook).

**`bb_cache_reactive_update(const bb_cache_update_t *req)`** wraps `bb_cache_update()`: after a write that actually changed the stored value, it fetches the key's serialized `{ts_ms,data}` envelope and fires every matching observer's `on_change(key, json, len, ts_ms, ctx)` — `ts_ms` lifted out of the envelope, `json`/`len` pointing at the `"data"` bytes only (not NUL-terminated; use `len`).

**Reentrancy (load-bearing).** The matching observer list is snapshotted under an internal lock, the lock is released, and only then are callbacks invoked — mirrors `bb_cache_foreach`'s snapshot-then-notify shape. `on_change` callbacks may safely call `bb_cache_*` or register a new observer, but must not re-enter `bb_cache_reactive_update()` for the same key. This layer only ever calls `bb_cache`'s public API — it never holds a raw `bb_cache` entry pointer, and its own observer lock is never held while calling into `bb_cache`.

**WS wiring (`bb_sink_ws`).** When the Kconfig is enabled, `bb_sink_ws` registers an observe-all `bb_cache_reactive` observer whose `on_change` pushes an immediate delta frame to subscribed WS clients (same hoisted `{type,topic,ts_ms,data}` shape as the periodic tick push — both paths coexist, delta is additive not a replacement). It also registers a `bb_websocket` connect callback (`bb_websocket_set_connect_cb`, mirrors the existing disconnect hook) that unicasts every currently-cached key's current value to a newly-connected client only (never a broadcast) via `bb_cache_foreach` + `bb_cache_get_serialized` — snapshot-on-connect. When the Kconfig is off, WS falls back to periodic-only push with zero reactive-layer cost.

## bb_sub — ingress router (bb_cache passthrough)

`bb_sub` is the single entry point for ingress data (MQTT-subscribed messages, or any external push) landing in `bb_cache`. `bb_sub_route(topic, payload, len)` registers the topic in `bb_cache` on first use (getter-mode, backed by `bb_sub`'s own fixed-size raw-JSON buffer) and updates it on every call thereafter — a thin passthrough, not a transform.

**Note the contrast with `bb_cache`'s owned-mode topics:** `bb_sub` stores the **raw JSON payload bytes** per topic in a fixed-size buffer (`BB_SUB_MAX_PAYLOAD_BYTES`, default 1024) — it is NOT compact/struct-shaped like an owned-mode `bb_cache` entry; `bb_cache_get_raw()` (owned-mode only) does not apply to `bb_sub`-routed topics.

**Aggregate event.** Every successful `bb_sub_route()` call also posts to the `"bb_sub.updated"` `bb_event` topic (`BB_SUB_EVENT_TOPIC`) carrying the routed topic name — a single small fan-out point for "something changed" notifications, independent of the per-topic `bb_cache` state itself.

## bb_alert — one-shot alert event channel

`bb_alert` is an opt-in component for emitting discrete, one-shot alert events on the `"alert"` `bb_event` topic (non-retained, non-replay). It is designed for transient failure or state-change signals; durable state belongs in retained `bb_cache` topics.

**Opt-in:** `CONFIG_BB_ALERT_ENABLE` (default `n`). When disabled, `bb_alert_emit()` compiles to a safe `static inline` no-op at zero cost. Consumers can call `bb_alert_emit()` unconditionally without `#ifdef` guards.

**Component location:** `components/bb_alert/` (header + Kconfig + CMakeLists). Shared implementation: `platform/host/bb_alert/bb_alert.c` (compiled host + espidf). ESP-IDF registry glue: `platform/espidf/bb_alert/bb_alert_espidf.c`.

**Topic:** `"alert"` — non-retained. Registered and attached at regular-tier order 4 (after `bb_event_routes` at order 0) when `CONFIG_BB_ALERT_AUTOREGISTER=y` (depends on `BB_ALERT_ENABLE && BB_EVENT_ROUTES_AUTOREGISTER`).

**API:**
- `bb_alert_emit(type, sev, fill, ctx)` — emit a one-shot alert. `type` is a string label (e.g. `BB_ALERT_TYPE_WIFI_LOST_IP`). `sev` is `BB_ALERT_INFO` / `BB_ALERT_WARNING` / `BB_ALERT_CRITICAL`. `fill` is an optional `bb_alert_fill_fn` callback that adds extra fields to the JSON object. `ctx` is passed through to `fill`.
- `bb_alert_register()` — registers the `"alert"` topic; called automatically by the registry init or manually in test teardown.

**Severity threshold:** `BB_ALERT_MIN_SEVERITY` (Kconfig default 0 = INFO, C default matches). Alerts below the threshold are silently dropped before any JSON allocation. In test builds, override with `bb_alert_set_min_severity_for_test(sev)`.

**Envelope:** `{type: string, severity: int, uptime_ms: int, ...fill fields}`. The JSON is serialized with `bb_json` and posted via `bb_event_post`.

**Consumers:**
- `wifi_lost_ip` (BB_ALERT_WARNING) in `platform/espidf/bb_wifi/wifi_reconn.c`: fired in `wifi_reconn_on_lost_ip()` and in the `ST_IDLE` no-IP watchdog path. Extra fields: `count`, `reason` (99 = `WIFI_REASON_BB_LOST_IP` sentinel), `retry_count`.
- `update_failure` (BB_ALERT_WARNING) in `components/bb_ota_check/src/bb_ota_check_common.c`: fired on fetch-failed and parse-failed paths (both streaming and custom parser). Extra fields: `current`, `latest`.
- `update_available` (BB_ALERT_INFO, transition edge only): fired when `available` transitions from `false` → `true`. Extra fields: `current`, `latest`.
- `update_applied` (BB_ALERT_INFO) in `bb_ota_check_mark_check_on_apply()`: fired when boot-mode OTA marks status as `check_on_apply` (staged/pending-reboot). Extra fields: `current`, `latest`.

**Convention:** durable state → retained `bb_cache` topics; discrete one-shot events → `bb_alert`.

## Event bus (bb_event, bb_event_ring, bb_event_routes)

`bb_event` is a portable callback-list publish/subscribe event bus. On ESP-IDF, subscribers receive events via a FreeRTOS dispatcher task; on Arduino, the app pumps events from `loop()` via `bb_event_pump()`. `bb_event_ring` is a sibling component providing a circular buffer with replay-on-subscribe — designed for fan-out scenarios (SSE/WebSocket subscribers, persistent event history). Both use the same `bb_event_t` opaque handle and dispatch model; `bb_event_ring` layers replay semantics on top of `bb_event` internally.

`bb_event_routes` exposes `GET /api/events` as a Server-Sent Events stream. Producers call `bb_event_routes_attach("topic.name")` to surface a topic on the stream; the route fans out to multiple concurrent clients, each with its own replay-on-connect via `bb_event_ring`. The payload contract is **caller posts valid UTF-8 JSON** — the route emits it raw in the SSE `data:` field. Arduino targets ship a 503 stub today (CC3000 RAM constraints); ESP-IDF is the production path.

**SSE peer-abort teardown requires `CONFIG_LWIP_SO_LINGER=y` (B1-517).** When an SSE client disappears without a graceful unsubscribe, `bb_sse_writer` detects the dead peer (`bb_http_req_peer_alive`, MSG_PEEK polling) and calls `bb_http_req_async_abort()` instead of the normal `bb_http_req_async_handler_complete()`. `bb_http_req_async_abort` arms `SO_LINGER{on,0}` on the socket so the eventual close triggers a RST on httpd's next session-cleanup pass instead of a graceful FIN exchange that parks the PCB in CLOSE_WAIT. **Consumers must set `CONFIG_LWIP_SO_LINGER=y` in their own `sdkconfig` to get this RST-based teardown.** A build missing the Kconfig gets a `#warning` from `platform/espidf/bb_http/bb_http.c` pointing back here — **this is a hard build error, not a soft one, for any consumer building under `-Werror`** (the smoke build and typical consumers do), because `-Werror` promotes the `#warning` to fatal. Only a build *without* `-Werror` degrades gracefully: it just warns, and at runtime the `setsockopt` fails harmlessly (logged, non-fatal) — only the faster peer-abort *detection* is active, not the RST-based PCB free. All breadboard smoke sdkconfigs that exercise SSE (`examples/smoke/sdkconfig.defaults.{esp32,esp32c3,tdongle,elecrow-p4-hmi7}`) set this.

**TaipanMiner re-pin note: TM MUST set `CONFIG_LWIP_SO_LINGER=y` when re-pinning to a breadboard release containing B1-517, or its `-Werror` CI build breaks on the `#warning` above.**

**`_ex` variants and the retained flag.** Both `bb_event_ring_attach` and `bb_event_routes_attach` have `_ex` variants that accept a `bool retained` parameter:
- `bb_event_ring_attach_ex(topic, capacity, max_entry, retained, out)` — preferred form; `bb_event_ring_attach` is `retained=false`.
- `bb_event_routes_attach_ex(topic_name, retained)` — preferred form; `bb_event_routes_attach` is `retained=false`.

Set `retained=true` for **state topics** (topics that publish current state, not discrete events). The flag documents intent and reserves API surface for future drain semantics. The practical effect today: callers that set `retained=true` MUST also publish an initial snapshot at component init so the ring is non-empty from T=0 — this ensures SSE clients connecting before the first periodic post receive the last known value rather than empty state. **Exception:** one-shot topics with no meaningful idle value (e.g. `ota.progress`, see Auto-attach convention below) are exempt — publishing a stale snapshot to a client connecting long after boot would be incorrect.

**SSE client subscription filtering.** `GET /api/events?topic=<name>` filters incoming SSE stream to a specific topic. The filter is applied server-side at client acquire time: `bb_event_routes_client_acquire_ex(out, topic_filter)` subscribes only to topics matching the filter, saving dispatcher work and reducing per-client queue pressure. Pass `NULL` to subscribe to all attached topics (equivalent to `bb_event_routes_client_acquire`). ESP-IDF clients extract the query param and pass it to `bb_event_routes_client_acquire_ex`; if the filter doesn't match any attached topic, the client still acquires but receives only heartbeats.

**Auto-attach convention.** A component that emits a `bb_event` topic owns its own attach to `bb_event_routes`, gated by `CONFIG_BB_<NAME>_AUTO_ATTACH` (`depends on BB_EVENT_ROUTES_AUTOREGISTER`). Consumers enabling both components get the topic on `/api/events` without an explicit `bb_event_routes_attach()` call. Disable the flag if you want the topic available for internal subscribers only. `bb_ota_check` follows this pattern (`update.available`) using `bb_event_routes_attach_ex(..., true)`; `bb_ota_hooks` follows the same shape for `ota.progress`, which is **retained** (B1-546, was non-retained) — the ring holds only the single most recent progress update, so a client connecting mid-OTA replays the last known state instead of empty; future emitters (`mining.share.*`) will follow the same shape. **Hardware-specific satellites** (`bb_pub_fan`, `bb_pub_power`, `bb_pub_thermal`, `bb_display_info`) default **n** — opt-in per board (enable only where the hardware exists, to avoid phantom topics on boards that lack the fan/power VRM/thermal sensors/display panel). **Universal/feature satellites** (`bb_pub_info`, `bb_pub_wifi`, `bb_ota_check`) keep **default y**.

**Topic discovery and diagnostics.** `GET /api/diag/events` lists every topic currently attached to `/api/events` with ring-buffer diagnostics: `ring_capacity`, `ring_count` (0 → no replay possible), `last_id`, `last_post_age_ms` (age of the last ring entry in ms, computed at request time; 0 if no events captured), and `last_size`. Top-level fields `max_clients` and `active_clients` show the configured concurrency cap and how many SSE connections are live. `slot_reuse_deferred` (B1-492) counts SSE connect attempts fast-rejected with 503 because the task-bundle reap-gate (below) had not yet confirmed the prior occupant's task `eSuspended` — distinct from a `max_clients`-exhaustion 503; a steady non-zero rate under reconnect churn is expected transient pressure (`EventSource` auto-retries), not a stuck pool. `pool_ensure_deferred` (B1-561) counts SSE connect attempts fast-rejected with 503 because the lazy first-connect task-bundle pool allocation (`sse_task_bundles_ensure()`) failed under transient heap pressure — a third, distinct 503 cause from both `slot_reuse_deferred` (reap-gate) and genuine `max_clients` exhaustion; the 503 body for this case is `{"error":"busy"}` rather than `{"error":"max_clients"}` so it is also distinguishable client-side. This is the first step when SSE shows `: connected` but no replay data — confirm the topic is attached and `ring_count > 0` before investigating the producer.

**Platform split (ESP-IDF components).** `bb_event_ring` and `bb_event_routes` are portable components with no ESP-IDF dependencies in their `components/` CMakeLists. ESP-IDF platform implementations live in standalone components under `platform/espidf/`:
- `platform/espidf/bb_event_ring_espidf/` — provides `bb_event_ring_now_us()` backed by `esp_timer_get_time()`. REQUIRES `bb_event_ring esp_timer`.
- `platform/espidf/bb_event_routes_espidf/` — registers `/api/events` + `/api/diag/events` HTTP routes. REQUIRES `bb_event_routes` and all ESP-only deps (`bb_json`, `esp_http_server`).

ESP-IDF consumers must add both directories to `EXTRA_COMPONENT_DIRS` and include `bb_event_ring_espidf` and `bb_event_routes_espidf` in their component's `REQUIRES` list (or the app's `idf_component_register` REQUIRES). The portable `bb_event_ring` and `bb_event_routes` components declare no `esp_timer`, `esp_http_server`, or `bb_json` dependencies.

**Heap budgets and Kconfig defaults.** At defaults `BB_EVENT_ROUTES_MAX_CLIENTS=2` and `BB_EVENT_ROUTES_QUEUE_DEPTH=8`, each client uses ~2.7 KB (8×256 B payload buf + 8×24 B queue entry overhead + ~88 B mutex). Consumers with more heap can override via `sdkconfig`. **`CONFIG_BB_EVENT_ROUTES_RING_CAPACITY`** (per-attached-topic non-retained replay ring depth) defaults to **8** (was 16 prior to B1-546) — right-sized against observed replay-on-connect usage; retained (state) topics are unaffected, always sized via the separate `CONFIG_BB_EVENT_ROUTES_RETAINED_RING_CAPACITY` (default 1). See `components/bb_event_routes/Kconfig`.

**SSE pool backing (`CONFIG_BB_EVENT_ROUTES_POOL_STATIC`, B1-491/B1-492).** The SSE per-client pool (queue entries + payload buffers in `bb_event_routes_common.c`, plus on ESP-IDF the writer task stack + TCB in `bb_event_routes_espidf.c`) is always index-addressed — client slot i deterministically owns pool bundle i, no free-list, no per-connect malloc/free churn (B1-478 PR E). This knob selects the pool's *backing*, not whether pooling happens:
- **n (default) — lazy heap-backed + idle-reclaimed.** Both arenas (payload/entries pool and, on ESP-IDF, the task-stack `bb_pool`) are allocated via a single contiguous heap allocation (SPIRAM-preferred) on the FIRST SSE client connect — a board that never gets an SSE client (headless, no dashboard) pays ≈0 standing RAM. A failed allocation (fragmented/low heap) fails soft: the connection is rejected and retried on the next connect, never a crash or unchecked NULL. **The ESP-IDF task-stack pool is torn down again once fully idle** (B1-492, superseding B1-491's "never freed once created"): `bb_event_routes_start()`'s PRE_HTTP idle-reclaim tick (`CONFIG_BB_EVENT_ROUTES_IDLE_RECLAIM_MS`, default 20 s) periodically reaps corpse tasks that have reached `eSuspended` and, once 0 active clients + 0 acquired bundles + 0 pending corpses remain, calls `bb_pool_destroy()` — returning the task-stack pool to 0 standing bytes, recreated lazily on the next connect. This never risks the B1-484/PR-E-CRITICAL use-after-free the eager-BSS design exists to avoid: destroy only fires once every bundle has been confirmed fully released and reaped (see the acquire-release lifecycle paragraph below) — the reclaim tick itself never gates or forces reuse, it only reclaims memory nothing is using anymore. The portable payload/entries arena in `bb_event_routes_common.c` is a separate allocation and is NOT reclaimed by this tick (unchanged from B1-491) — only the ESP-IDF task-stack `bb_pool` is.
- **y — permanent static-BSS arena** (the pre-B1-491 PR E shape): carved once at boot from a fixed BSS buffer, ~13.5 KB at Kconfig defaults on ESP32-C3, never allocated or freed at runtime. Deterministic and zero-heap, at the cost of that fixed BSS reservation — generally not affordable on the tightest no-PSRAM boards (ESP32-C3, ~18–25 KB free). The idle-reclaim tick is **not created at all** in this mode — an eager-BSS pool has nothing to reclaim, so `bb_event_routes_start()` is a deliberate no-op rather than a timer that spins doing nothing.

**`bb_pool` acquire-release lifecycle idiom (B1-479/B1-492).** `bb_pool` SLOTS mode's optional `slot_reusable`/`slot_reap` callbacks (`bb_pool.h`) implement an async reuse-readiness gate for objects whose teardown outlives the caller's `bb_pool_release()` call — the SSE task-stack pool's canonical use: a released bundle's task hasn't necessarily reached `eSuspended` yet, so it is held "pending" rather than immediately reissued. Three read-only accessors let a consumer reason about a SLOTS pool's aggregate lifecycle state without acquiring/disturbing any slot: `bb_pool_slots_acquired_count()` (slots currently on loan), `bb_pool_slots_pending_count()` (released but not yet confirmed reusable), and `bb_pool_slots_reap_ready()` (a proactive GC pass — reaps every currently-ready pending slot onto the free-list, without acquiring/handing any of them to a caller). **A pool is only truly idle — safe to `bb_pool_destroy()` — when all three of "no external users," acquired_count==0, and pending_count==0 hold simultaneously.** Checking only two of the three reopens a use-after-free window: an object mid-teardown (its owner has already stopped counting as an "external user" but has not yet called `bb_pool_release()`) is neither an external user nor a pending corpse — it is still *acquired*, i.e. a task may still be executing on the very memory about to be freed. `bb_event_routes_espidf.c`'s idle-reclaim tick checks `bb_event_routes_active_client_count() == 0 && bb_pool_slots_acquired_count() == 0 && bb_pool_slots_pending_count() == 0` (via the pure `sse_pool_reclaim_decide()` predicate, `components/bb_event_routes/src/sse_pool_reclaim_decision.c`) for exactly this reason.

**Transport-neutral rule: internal consumers subscribe in-process, never hold an egress slot.** The SSE task-stack pool's `bb_pool_acquire()`/`bb_pool_release()` lease and its reap-gate/idle-reclaim contract are written generically enough that a future WS egress consumer (B1-514) can reuse them unchanged — but that only holds if every consumer of a `bb_event`/`bb_event_ring` topic that lives *inside* the same process (e.g. a future in-process dashboard aggregator, or a satellite computing a derived metric from another topic) subscribes via the normal `bb_event` callback API directly, never by opening an actual `/api/events` HTTP connection to itself and occupying one of the finite egress pool slots (`CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS`, default 2). An in-process consumer competing with real external SSE/WS clients for the same small pool defeats the whole reason the pool is capacity-bounded in the first place.

**`bb_event_routes_start()` PRE_HTTP placement (B1-492).** Declared in `bb_event_routes.h` (ESP-IDF only, `#ifdef ESP_PLATFORM`), implemented in `bb_event_routes_espidf.c`, self-registered via `BB_INIT_REGISTER_PRE_HTTP(bb_event_routes_start, bb_event_routes_start)` + the matching `bb_init_force_register_pre_http(${COMPONENT_LIB} bb_event_routes_start)` in `platform/espidf/bb_event_routes_espidf/CMakeLists.txt` — mirrors `bb_pub_start()` (`platform/espidf/bb_pub/bb_pub_espidf.c`) and the `12de73d` "decouple background-evaluator start from route-attach" convention. It does state-init + timer-create + timer-arm ONLY (via `bb_timer_deferred_periodic_create`, per the Timer callback convention below — `bb_pool_destroy()` frees memory and must never run on the esp_timer service task). `bb_event_routes_register_routes_init` (REGULAR tier, unchanged) stays pure httpd route-attach with no timer/task creation, per the "Route/SSE-attach functions are pure httpd" rule. `bb_event_routes_start()` is idempotent — a second call while the reclaim timer is already armed (`s_sse_reclaim_timer != NULL`) returns `BB_OK` immediately without arming a duplicate timer; unlike `bb_pub_start()` (fully static, single self-registered caller), this function is public and externally callable. **Accepted public-header exception:** `bb_event_routes_start()` is declared inside `#ifdef ESP_PLATFORM` in the public `bb_event_routes.h` — platform-only functions with portable (no platform-typed) signatures may be declared under `#ifdef ESP_PLATFORM` in a public header (precedent: `bb_wifi.h` gateway/ping APIs).

**Dispatch pool and per-client queue allocation.** The dispatch pool and per-client queues prefer SPIRAM on ESP-IDF (overridden at platform init with fallback to default heap). `bb_event_ring` per-topic rings also use SPIRAM-preferred allocation on ESP-IDF.

**Tunables (from `#296`).** `BB_EVENT_MAX_TOPICS` default 8; `BB_MDNS_EVT_POOL_SIZE` default 8; `BB_DIAG_PANIC_BUF_SIZE` default 1024 (RTC slow). New: `BB_LOG_TAG_REGISTRY_MAX` (default 24), `BB_HTTP_ROUTE_REGISTRY_CAP` (default 64), `BB_MDNS_TXT_PENDING_MAX` (default 4), `BB_OTA_CHECK_CUSTOM_PARSER_BUF_BYTES` (default 8192).

## Update check (bb_ota_check, bb_release_manifest, bb_http_client)

`bb_ota_check` periodically polls a release-manifest URL, compares the returned version against `bb_system_get_version()`, and on a state change posts the `update.available` `bb_event` topic and updates the mDNS TXT key `update=<value>` (`unknown` pre-check, `none` up to date, `<tag>` when newer is available).

- **Initial snapshot:** `bb_ota_check_init` does NOT post one; callers must call `bb_ota_check_publish_initial()` after attaching a ring/routes with `retained=true`. The ESP-IDF platform impl (`bb_ota_check_register_init`) does this automatically under `CONFIG_BB_OTA_CHECK_AUTO_ATTACH`. Initial snapshot: `available=false`, `current=<running version>`, `latest=""`, `download_url=""`, `last_check_ok=false`.
- **Parser:** `bb_ota_check_set_releases_url(url)` + optionally `bb_ota_check_set_parser(fn)`. Default parser is `bb_release_manifest_parse_github`; custom parsers supply `bb_release_manifest_parse_fn`.
- **Transport:** fetches via `bb_http_client` (same wrapper as `bb_ota_pull`).
- **Routes:** auto-registers `GET /api/update/status`; ESP-IDF only — Arduino stubs every setter to `BB_ERR_UNSUPPORTED`.

**Runtime opt-out.** `bb_nv_config_update_check_enabled()` / `bb_nv_config_set_update_check_enabled(bool)` mirror the `_mdns_enabled` pattern: the getter returns `true` by default; the setter persists to NVS on ESP-IDF (in-memory only on host). The periodic timer still fires on its interval, but `bb_ota_check_run_one` no-ops and returns `BB_OK` when the flag is `false`. `GET /api/update/status` includes an `"enabled"` boolean field that reflects the current NV setting. The compile-time `CONFIG_BB_OTA_CHECK_AUTOREGISTER` Kconfig is unchanged; both compile-time and runtime opt-outs are independent.

**Firmware board name.** `bb_ota_check_set_firmware_board(board)` overrides the asset name prefix used when matching release assets. Default fallback is `"unknown"` (looks for `unknown.bin`). Pass the board prefix without `.bin` (e.g. `"taipanminer-tdongle-s3"` → `taipanminer-tdongle-s3.bin`). Pass `NULL` or `""` to revert to the default. `bb_ota_check_get_status().board` is the single source of truth for the effective board name. `bb_ota_pull` reads from `bb_ota_check_get_status()` — **`bb_ota_pull_set_firmware_board` has been removed**; use `bb_ota_check_set_firmware_board` only. Returns `BB_ERR_INVALID_STATE` before init, `BB_ERR_INVALID_ARG` if string exceeds 63 chars.

**Streaming fetch path.** The default GitHub parser is invoked via the streaming API: `bb_http_client_get_stream` feeds 2 KB chunks into `bb_release_manifest_parse_github_stream_feed`. Only parser state (~400 B on stack) and the two extracted strings live in memory. Custom parsers registered via `bb_ota_check_set_parser` fall back to a heap allocation per fetch (size tunable via `CONFIG_BB_OTA_CHECK_CUSTOM_PARSER_BUF_BYTES`, default 8192 bytes). `bb_ota_pull` uses `bb_ota_check_get_status()` for the cached result; `ota_fetch_manifest` is still compiled for the `BB_OTA_PULL_TESTING` hook but is not called on device.

**`bb_ota_check` is the single source of truth for manifest checks.** `bb_ota_check_get_status(bb_ota_check_status_t *out)` copies the cached status snapshot under the internal pthread mutex. Callers do not hold any lock. `POST /api/update/apply` reads this to extract `latest` and `download_url` for the OTA worker; returns 503 if `last_check_ok == false` (no recent successful check), 409 if `available == false`. `POST /api/update/check` calls `bb_ota_check_kick()` and returns `{"status":"checking"}` (200) immediately; callers poll `GET /api/update/status` for results.

**`bb_ota_check_now()` vs `bb_ota_check_kick()`:**
- `now()` — synchronous; runs manifest fetch + mbedTLS on caller's stack (needs ≥8 KB). Use from worker tasks and test harnesses.
- `kick()` — non-blocking on ESP-IDF; posts semaphore to wake worker task. Use from HTTP handlers (httpd workers ~4 KB). On host/Arduino, synchronous stub.

The streaming parser (`bb_release_manifest_parse_github_stream_{begin,feed,end}`) is a resumable byte-at-a-time state machine. Matches `assets[].name == "<board_fallback>.bin"` → takes that asset's `browser_download_url`.

**Pause/resume hooks.** `bb_ota_check_set_hooks(pause_fn, resume_fn)` brackets each manifest fetch. `pause_fn` is called just before `bb_http_client_get_stream`; `resume_fn` immediately after (both success and failure). Mirrors `bb_ota_pull_set_hooks`. Callback types are `bb_http_pause_cb_t` / `bb_http_resume_cb_t` from `bb_core.h`. Either argument may be NULL.

**Task stack budget.** Any task that calls `bb_http_client_get*` must allocate at least `BB_HTTP_CLIENT_TASK_STACK` bytes (8 KiB default, `CONFIG_BB_HTTP_CLIENT_TASK_STACK_SIZE`). The mbedTLS handshake + cert-bundle parse path needs 5–8 KiB; a smaller task stack overflows into adjacent heap blocks and corrupts heap metadata, surfacing as an unrelated assertion in the next `calloc`. Both `bb_ota_check` and `bb_ota_pull` reference the macro so the budget stays consistent.

## Provisioning UI

The `bb_prov` component manages the provisioning state machine and HTTP `/save` handler. Callers MUST supply at least one asset with `path="/"` to `bb_prov_start`. For bare-minimum bringup, add `REQUIRES bb_prov_default_form` and pass `&bb_prov_default_form_asset`. Custom UIs pass their own asset array instead. `POST /save` returns `204 No Content`; the caller's form JS is responsible for post-submit UX.

`bb_prov_start(assets, n, extra)` owns the full prov-mode route graph: it registers `/save`, assets, registry routes (e.g. `POST /api/scan`, `/api/reboot`, `/api/info`), an optional consumer `extra` callback, then the captive-portal `/*` GET wildcard — in that exact order so specific handlers always win first-match. Pass `NULL` for `extra` when the UI only needs the built-ins. Use `extra` for advanced UIs that need dynamic endpoints (e.g. live diagnostics, pool-test buttons); register them on `server` and bb_prov will sequence them correctly. Additional form fields (pool/wallet/worker/etc.) stay on the `/save` body and are parsed via `bb_prov_set_save_callback`.

**`POST /save` and `GET /*` are intentional exceptions to the described-route SSOT.** Both use bare `bb_http_register_route`, not `bb_http_register_described_route` — they exist only in provisioning mode, not the app's normal `/api/*` surface. `/save` takes a URL-encoded form body (not JSON), which doesn't fit the JSON-Schema `request_schema` field cleanly. `GET /*` is a captive-portal catch-all redirect (any unmatched path, used to trigger the OS captive-portal browser popup) — it has no fixed `path`, so it cannot be expressed as a described-route descriptor at all.

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
2. `bb_net_health_attach_sse()` — in the regular-tier init (after `bb_event_routes` is initialised). Attaches the `"net.health"` retained SSE topic, publishes an initial snapshot so the ring is non-empty from T=0, and starts a 5-second periodic evaluator that re-publishes on state or `early_warning` change. **Per-topic ring sizing (B1-472):** the attach uses `bb_event_routes_attach_ex2(..., BB_NET_HEALTH_SSE_MAX_ENTRY)` — a named constant (512, defined in `bb_net_health.h`) — because the serialized snapshot (nested `mqtt`/`http` objects, ~341–352 B) exceeds the `bb_event_routes` global default `max_entry` (256, `CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY`); a ring sized at the global default silently rejects the retained push (`bb_ring: push rejected: len=... > max_entry=256`) and SSE clients connecting to `?topic=net.health` see empty state. Same shape as the `update.available` / `info.build` precedent (#616, B1-434/435/439). **`CONFIG_BB_NET_HEALTH_SSE_AUTO_ATTACH`** (B1-546, `depends on BB_EVENT_ROUTES_AUTOREGISTER`, default **y**) gates the body of `bb_net_health_attach_sse()`: when disabled, the call is a safe no-op (logs at debug and returns `BB_OK`) — the `"net.health"` topic is never registered/attached and its retained-ring heap is never allocated. Gated inside the function (not at the call site) so TM's unconditional call to `bb_net_health_attach_sse()` needs no change; default y is zero behavioral change from pre-B1-546.

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

**This transient `/api/update/progress` route is an intentional described-route exception.** `boot_progress_server_start()` (`bb_ota_boot.c`) calls `bb_http_server_ensure_started()` + bare `bb_http_register_route` directly — it runs during the boot-mode download window, before `bb_init_init()` (and therefore before `bb_openapi`) ever runs, so there is no OpenAPI walker alive to consume a descriptor even if one were registered. The sibling boot-mode routes served during *normal* operation from the same file (`/api/update/apply`, `/api/update/status`, `/api/update/check`, registered from `bb_ota_boot_init` inside the regular `bb_init` tier) already use `bb_http_register_described_route` — only this early, pre-`bb_init` transient route is the exception, and only because of its lifecycle position.

There is no `/api/update/boot` verb — boot-mode arms via `/api/update/apply`. The per-component autoregister bools stay user-visible for the manual-registration escape hatch.

**Strategy↔update-check coupling.** `CONFIG_BB_OTA_CHECK_AUTOREGISTER` defaults off when `BB_OTA_STRATEGY_BOOT` is selected: boot-mode boards run the manifest check synchronously inside the boot-mode worker at full early-boot heap, so the recurring runtime task (~8 KB worker + HTTPS timer) is dead weight and cannot complete a TLS handshake on heap-tight boards. Pull-strategy boards keep the default `y`. Override to `y` explicitly if a boot-mode board also needs the runtime `GET /api/update/status` route.

**`check_on_apply` directive (heap-tight boot-mode boards).** When `CONFIG_BB_OTA_BOOT_STATUS_HTTP=y` and the on-demand `POST /api/update/check` fires but heap is below the TLS threshold, the default is 503 `{"error":"insufficient_heap"}`. Enabling `CONFIG_BB_OTA_CHECK_ON_APPLY_FALLBACK=y` (depends on `BB_OTA_BOOT_STATUS_HTTP`, default n) changes this: instead of the 503, the handler calls `bb_ota_check_mark_check_on_apply()` and returns 200 `{"status":"check_on_apply"}`. `GET /api/update/status` then reflects `outcome:"check_on_apply"`, `available:false`. This signals consumers to skip the runtime check and go straight to `POST /api/update/apply` — boot-mode always performs the full manifest check at early-boot heap where contiguous RAM is plentiful. The heap-OK path (real check → 202 `{"status":"checking"}`) is unchanged.

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

**`GET /api/diag/partitions` is owned by `bb_partition` (B1-456), not `bb_diag`.** It self-registers via `BB_INIT_REGISTER` in `platform/espidf/bb_partition/bb_partition_routes.c`, gated by `CONFIG_BB_PARTITION_ROUTES_AUTOREGISTER` (default y) — the same pattern `bb_event_routes` uses to own `/api/diag/events`. Emits the partition table (label/type/subtype/offset/size + running/next-OTA flags) from `bb_partition_list()`.

**`GET /api/diag/net` is owned by `bb_net_health` (B1-456), not `bb_diag`.** It self-registers via `BB_INIT_REGISTER` in `platform/espidf/bb_net_health/bb_net_health_routes.c`, gated by `CONFIG_BB_NET_HEALTH_ROUTES_AUTOREGISTER` (default y) — same satellite pattern as `bb_partition`'s ownership of `/api/diag/partitions`. Emits network diagnostic counters: `uptime_ms`, `http_handler_count`/`http_handler_cap` (from `bb_http`), and the `bb_net_health_get_status()` snapshot (rssi, disc/reconnect counters, nested `mqtt`/`http` objects).

**`GET /api/diag/net` is the single source of truth for wifi recovery counters (B1-486, BREAKING).** `no_ip_recoveries` (`bb_wifi_get_no_ip_count`), `lost_ip_recoveries`/`lost_ip_age_s`, `egress_dead_recoveries`, `recovery_count` (sum of the three), and `reason_histogram` (`lost_ip`/`egress_dead`/`no_ip_watchdog` sentinel buckets + `top_reason_code`/`top_reason_count`) live **only** on `/api/diag/net`. `GET /api/wifi` (both the `bb_wifi_routes.c` live-fallback emitter `bb_wifi_emit_section` and the `bb_pub_wifi` "wifi" telemetry topic that actually backs the route's `bb_cache` entry in normal operation) no longer emits any of these — it keeps connection-state fields only (`ssid`, `bssid`, `rssi`, `ip`, `connected`, `disc_reason`, `disc_age_s`, `retry_count`, `restart_sta_count`, `disconnect_rssi`). Previously `/api/wifi` and the "wifi" telemetry topic duplicated `no_ip_recoveries`/`egress_dead_count`/`lost_ip_count`/`recovery_count`/`reason_histogram` under different names than `/api/diag/net`'s `lost_ip_recoveries`/`egress_dead_recoveries` — dashboards/scripts reading those fields off `/api/wifi` or the "wifi" MQTT/HTTP/SSE telemetry topic must switch to `/api/diag/net`. The top-reason computation is a public `bb_wifi` API — `bb_wifi_reason_histogram_top(const uint16_t *hist, uint16_t *out_count)` (pure, host-testable; implemented once in `platform/host/bb_wifi/bb_wifi_emit.c`, compiled host+espidf) — rather than `bb_net_health_routes.c` reaching into a private `bb_wifi` header. The three breadboard sentinel reason codes are public named constants: `BB_WIFI_REASON_BB_LOST_IP`/`_EGRESS_DEAD`/`_NO_IP_WATCHDOG` (99/100/101, `bb_wifi.h`); the private `WIFI_REASON_BB_*` macros in `components/bb_wifi/wifi_reconn_policy.h` (the production histogram writer) alias these public constants. `bb_net_health_status_t.no_ip_recoveries` is captured in the same 5 s evaluator snapshot as `lost_ip_recoveries`/`egress_dead_recoveries` so `/api/diag/net`'s `recovery_count` sums point-in-time-consistent operands (not a live `bb_wifi_get_no_ip_count()` call mixed with a 5 s-stale snapshot).

**`GET /api/diag/net` transport-observability fields (B1-518 PR2/PR3, observe-only).** A live `"transports"` array (built at request time from `bb_transport_health_snapshot_all()`, empty — not omitted — when nothing is registered) reports per-transport health from `bb_transport_health`: `name`, `cls` (`"authoritative"` for sinks that report their own publish success/failure, e.g. `bb_sink_http`/`bb_sink_mqtt`; `"inferred"` otherwise), `enabled`, `failing`, `last_ok_ms`, `fail_count`, plus `last_rx_ms`/`rx_count` when `cls == "inferred"`. A top-level `"egress_state"` string (`"ok"`/`"endpoint_down"`/`"gw_unreachable"`/`"all_dead"`, from `bb_net_health_classify_egress`) attributes an egress fault to either the WiFi link (gw unreachable/all dead) or an upstream/application-layer problem (endpoint down, gw still reachable) — the gateway probe is the tiebreaker. The `net_state` log heartbeat gains two trailing tokens, each omitted when not applicable: `txfail=<failing>/<enabled>` (present iff at least one transport is registered) and `egr=<state>` (present iff `egress_state != "ok"`); both drop first under the line-truncation cap (`egr` before `txfail`, `txfail` before `gw`). **Boards with the gateway probe disabled (`CONFIG_BB_WIFI_GW_PROBE_ENABLE=n`, the default) never populate `gw_probed`, so `bb_net_health_classify_egress` cannot attribute a fault to the WiFi link and `egress_state` stays `"ok"` even while `transports[].failing` is true** — `egress_state` answers "is this a WiFi-link fault?" (requires gateway corroboration) while `transports[].failing` answers "is this transport currently failing?" (no corroboration needed); the two fields are expected to disagree on gw-probe-disabled boards and that is not a bug. This is observe-only — no recovery action is wired to `egress_state` yet.

**`GET /api/diag/tasks` includes `bb_task_registry` occupancy (B1-471, BREAKING).** The response root changed from a bare array to an object: `{"tasks": [...], "registry": {"count": N, "capacity": M, "dropped": D}}` — the `"tasks"` array is byte-identical to the prior array-at-root payload (still streamed true chunks via the obj-stream API); only the wrapping changed. `registry.count`/`capacity`/`dropped` come from `bb_task_registry_count()`/`bb_task_registry_capacity()`/`bb_task_registry_dropped()` and are independent of the live FreeRTOS task list above (a task can be self-registered without a matching `TaskStatus_t` entry, and vice versa). `bb_task_registry_dropped()` is a monotonic counter of `register()`/`test_seed()` calls rejected with `BB_ERR_NO_SPACE` because the pool (`BB_TASK_REGISTRY_MAX`, default 24) was full — previously only a one-line `bb_log_w` at the moment of the drop, now durably observable on-device. A one-shot `bb_log_w` high-watermark warning also fires from `bb_task_registry_register()`/`bb_task_registry_test_seed()`'s shared pool allocator when the pool comes within `BB_TASK_REGISTRY_HWM_MARGIN` (2) slots of capacity.

**Abnormal-reset counter semantics.** `abnormal_reset_count` (surfaced as `wdt_resets` in consumers) counts abnormal resets *since this firmware was deployed*, not a lifetime total. At each boot, the running app's ELF SHA256 is fingerprinted (first 4 bytes of the raw SHA256, stored in NVS key `app_fp` under the `bb_diag` namespace). If the stored fingerprint is absent (`0`) or differs from the running firmware, the counter is reset to 0 and the new fingerprint is stored — the deploy boot itself is the clean baseline and is not counted. Subsequent abnormal resets on the same build increment as before. Still explicitly clearable via `DELETE /api/diag/boot`. The decision logic is factored into the pure host-testable function `bb_diag_reset_decision()` in `components/bb_diag/bb_diag_reset_decision.c`.

**`GET /api/diag/boot` reboot-reason SSOT (B1-527 PR-A).** A nested `"reboot_reason"` object surfaces the *semantic* (app-level) cause of the last reboot, sourced from `bb_system_restart_reason()` (see the `bb_system` section below): `{"source": string, "detail": string (omitted when empty), "uptime_s": integer, "epoch_s": integer, "age_s": integer (omitted when epoch_s==0 or the current wall clock isn't NTP-synced)}`. This is a **single last-reboot record, cleared on read** — distinct from the `reboot_history` ring described below. At boot, `bb_diag` reads+decodes the record persisted under `BB_REBOOT_NVS_NS`/`BB_REBOOT_KEY_LAST`, then erases it immediately regardless of decode outcome; the decoded value is only trusted (kept as-is) when this boot's hardware reset reason is `BB_RESET_REASON_SW` — otherwise (e.g. a panic intervened before the intended reboot completed, or no record was ever written) `reboot_reason.source` reports `"unknown"` with `detail`/`epoch_s`/`uptime_s` all empty/zero. **`reboot_reason.source` can legitimately disagree with the top-level `reset_reason` field** — `reset_reason` is the hardware-level ESP-IDF `esp_reset_reason()` classification (`"software"` for any `esp_restart()` call, regardless of *why*), while `reboot_reason.source` names the app-level cause (e.g. `"api_reboot"`, `"egress_tier3"`) that triggered that `esp_restart()`. `age_s` is computed fresh on every `GET /api/diag/boot` call (and on every `bb_cache_post` to the retained `diag.boot` SSE topic) from the current wall clock, not memoized from publish time.

**`GET /api/diag/boot` reboot history ring (B1-527 PR-B).** A `"reboot_history"` array surfaces the last `BB_REBOOT_HISTORY_CAP` (8) reboots, **newest-first**: `[{"source": string, "epoch_s": integer, "uptime_s": integer}, ...]` — minimal fields only (no `detail`, no `age_s`; a consumer computes age from `epoch_s` itself). Empty array when nothing has been captured yet. Unlike `reboot_reason` above, this ring is **NOT cleared-on-read** — it accumulates across boots — and it captures **every** boot this firmware sees, including untagged/hardware resets (recorded as `source: "unknown"`, `epoch_s: 0`, `uptime_s: 0`), not just app-requested reboots that went through `bb_system_restart_reason()`. Persisted under `BB_REBOOT_NVS_NS`/`BB_REBOOT_KEY_HISTORY` as one delimited string via `bb_reboot_history_encode`/`_decode` (`components/bb_core/src/bb_reboot_reason.c`, pure/host-testable, `bb_system.h`). At boot, `bb_diag`'s `load_reboot_record()` (same function that handles the single record above) reads+decodes the existing ring, pushes exactly one entry for this boot mirroring the effective `reboot_reason` just computed, then re-encodes and persists — one NVS write per boot, same cost class as the abnormal-reset-count write.

**bb_diag_boot_snap_t / bb_diag_boot_serialize (pure, host-testable).** The shared serializer (`components/bb_diag/bb_diag_event_common.c`) backs both `GET /api/diag/boot` and the retained `diag.boot` SSE topic. `reboot_reason` is always emitted (source defaults to `"unknown"`); `detail` is omitted when empty; `age_s` is omitted whenever `reboot_epoch_s == 0` or the snapshot's `now_epoch_valid` flag is false (set by the ESP-IDF caller from `bb_ntp_is_synced()` + `time(NULL)`, refreshed immediately before every publish/serialize call — see `build_boot_snap()` in `platform/espidf/bb_diag/bb_diag_routes.c`) or `now_epoch_s < reboot_epoch_s` (clock-skew guard against a negative age). `reboot_history` is always emitted as an array (possibly empty), newest-first, walking the ring backwards from the most recently pushed slot.

## bb_version — build-time firmware version identifier

`bb_system_get_version()` returns `BB_FW_VERSION_STR` when the generated header is
present (via `#if __has_include("bb_version_gen.h")`), otherwise falls back to
`esp_app_desc.version`. The version string is generated at build time by breadboard's
PlatformIO hook (`scripts/bbtool_pio.py`) or `cmake/bbtool.cmake`. See
`scripts/bbtool/README.md` for the precedence ladder, wiring, and fail-soft behavior.

## bb_system — reboot-reason SSOT (B1-527 PR-A)

`bb_system_restart_reason(bb_reset_source_t src, const char *detail)` is the preferred way for any component to intentionally reboot the device — it persists a semantic reboot reason to NVS (`BB_REBOOT_NVS_NS`/`BB_REBOOT_KEY_LAST`, single last-reboot record, cleared on read by `bb_diag` at boot — see the `bb_diag` section) before calling `esp_restart()`. `detail` may be `NULL`; non-NULL values are bounded to 48 chars and truncated at the first `|` (the record's delimited encoding reserves `|` as a field separator). `bb_reset_source_t` is a closed portable enum (`BB_RESET_SRC_UNKNOWN`, `_API_REBOOT`, `_FACTORY_RESET`, `_WIFI_SAFEGUARD`, `_WIFI_COLD_TIMEOUT`, `_WIFI_PENDING_REVERT`, `_WIFI_RECONFIGURE`, `_EGRESS_TIER3`, `_OTA_PULL_APPLIED`, `_OTA_PUSH_APPLIED`, `_OTA_BOOT_APPLY`, `_OTA_BOOT_DONE`, `_OTA_BOOT_ABORT`) — a new intentional-reboot site adds a value here rather than calling `bb_system_restart()`/`esp_restart()` directly. `bb_reset_source_str(src)` returns the stable wire string (e.g. `"egress_tier3"`); out-of-range values map to `"unknown"`. Both the enum-to-string mapping and the record pack/unpack (`bb_reboot_record_t` + `bb_reboot_record_encode`/`bb_reboot_record_decode`, `components/bb_core/src/bb_reboot_reason.c`) are pure and compiled on every platform (host/ESP-IDF/Arduino), 100% branch-covered by host tests. The plain `bb_system_restart()` (no reason) is unchanged and still used where no semantic classification applies (e.g. Arduino WiFi reconnect paths) and by the generic `bb_system_restart()` primitive itself.

**PR-C (B1-527): all intentional reboot sites migrated.** Every deliberate `esp_restart()` call site across the espidf platform tree now funnels through a semantic reboot-reason tag: `POST /api/reboot` (`BB_RESET_SRC_API_REBOOT`), `bb_net_health` egress tier-3 (`BB_RESET_SRC_EGRESS_TIER3`), `wifi_reconn`'s persistent-disconnect safeguard reboot (`BB_RESET_SRC_WIFI_SAFEGUARD`, detail `"persistent disconnect"`), `bb_wifi`'s 60s cold-connect timeout (`BB_RESET_SRC_WIFI_COLD_TIMEOUT`, detail `"cold connect timeout"`), `PATCH /api/wifi` reconfigure-apply (`BB_RESET_SRC_WIFI_RECONFIGURE`), pending-credential revert on a failed reconfigure try (`BB_RESET_SRC_WIFI_PENDING_REVERT`), in-place OTA pull success (`bb_ota_pull`, `BB_RESET_SRC_OTA_PULL_APPLIED`), OTA push success (`bb_ota_push`, `BB_RESET_SRC_OTA_PUSH_APPLIED`), boot-mode OTA (`bb_ota_boot`): `/api/update/apply` arm (`BB_RESET_SRC_OTA_BOOT_APPLY`), successful pull-and-reboot-to-new-image (`BB_RESET_SRC_OTA_BOOT_DONE`), and every boot-mode abort path collapsed onto one source with a `detail` tag distinguishing the cause — `BB_RESET_SRC_OTA_BOOT_ABORT` with detail `"init_fail"` / `"manifest_fail"` / `"no_update"` / `"pull_fail"` / `"no_wifi"` / `"task_fail"` — and `POST /api/factory-reset` (`BB_RESET_SRC_FACTORY_RESET`, B1-532). Factory-reset now tags via `bb_nv_reboot_record_save()` directly rather than `bb_system_restart_reason()`: `bb_reset_source_t` and the reboot-record codec moved to `bb_core` (B1-532 PR1), so `bb_nv` persists the tagged record without depending on `bb_system` — no component cycle. **The reboot-reason SSOT is complete: every intentional `esp_restart()` site in the espidf platform tree now carries a semantic reason.** The generic `bb_system_restart()` primitive itself (`platform/espidf/bb_system/bb_system.c`) still stays a bare `esp_restart()` wrapper by design — it has no semantic reason to tag.

**`ota_boot_stg` breadcrumb retired (B1-527 PR-C).** `bb_ota_boot`'s old progress-checkpoint NVS key (`OTA_BOOT_STAGE_KEY` = `"ota_boot_stg"`, values 1/2/3/4/8/0xE1-0xE5) was write-only — never read by any consumer — and is now superseded by the `BB_RESET_SRC_OTA_BOOT_APPLY`/`_DONE`/`_ABORT`+`detail` reboot-reason tagging above, which carries strictly more information (a stable wire string plus a cause detail) than the old numeric stage codes. The `breadcrumb()` helper, its call sites, and the `OTA_BOOT_NS`/`OTA_BOOT_STAGE_KEY` `#define`s have been removed.

**Caller-supplied timestamp/detail for `POST /api/reboot` (B1-527 follow-up).** `bb_system_restart_reason_at(bb_reset_source_t src, const char *detail, uint32_t caller_epoch_s)` is a variant of `bb_system_restart_reason` that accepts a caller-supplied epoch — for a board with no NTP sync, this lets the record still carry a real `epoch_s` instead of 0. `bb_system_restart_reason(src, detail)` is unchanged (still the preferred call for every autonomous reboot site listed above) and is now a one-line wrapper: `bb_system_restart_reason_at(src, detail, 0)`. Epoch selection is a pure, host-tested helper: `bb_reboot_pick_epoch(ntp_synced, device_epoch_s, caller_epoch_s, floor_s)` (`components/bb_core/src/bb_reboot_reason.c`) — device NTP time wins whenever it is synced and at/above the sanity floor (`1704067200` = 2024-01-01 UTC); otherwise the caller epoch is used if it clears the same floor; otherwise `0`. `POST /api/reboot` is the only caller of `_at` today: its handler accepts an optional JSON body `{"ts": <epoch_s>, "detail": "<string, up to 48 chars>"}` — both fields optional, tolerating no body / an empty body / non-JSON (all fall back to `ts=0`, `detail=""`, matching the pre-existing behavior). **`detail` precedence: body `detail` (non-empty) > request `User-Agent` header > `""`.** Every other (autonomous) reboot site is unchanged — they all still call the plain `bb_system_restart_reason(src, detail)` with `caller_epoch_s` implicitly 0.

**`bb_system_reboot_parse_body` — pure, host-testable body parse (B1-527 follow-up, firmware-review fix).** The `ts`/`detail` extraction and precedence logic is factored out of the ESP-IDF-only route handler into `void bb_system_reboot_parse_body(const char *body, int body_len, const char *ua_or_null, uint32_t *out_ts, char *out_detail, size_t out_detail_len)` (`components/bb_system/src/bb_system_reboot_parse.c`, declared in `bb_system.h`) — pure aside from `bb_json`, compiled on host/ESP-IDF/Arduino, 100% branch-covered by host tests. It takes the already-resolved User-Agent string as a plain parameter (not a request handle), so it has no platform dependency and is directly host-testable — this closes the gap where the original inline parsing logic lived only in the espidf-only route file with zero host coverage. `ts` is parsed from `body["ts"]` and clamped to `(0, UINT32_MAX]` before the `uint32_t` cast (negative, zero, NaN/Inf, and >UINT32_MAX all yield `out_ts=0` — this is also what closes the ts-cast UB risk). `detail` precedence (body `detail` non-empty > `ua_or_null` non-NULL/non-empty > `""`) is evaluated inside this pure function. `out_ts`/`out_detail` must be non-NULL and `out_detail_len` non-zero — an early guard returns immediately otherwise (no OOB write on a zero-length output buffer). `reboot_handler` (`platform/espidf/bb_system/bb_system_routes.c`) is now a thin wrapper: read the bounded/NUL-terminated body, resolve `ua_p` via `bb_http_req_get_header(req, "User-Agent", ua, sizeof(ua)) == BB_OK ? ua : NULL`, call `bb_system_reboot_parse_body`, then `bb_system_restart_reason_at`. The respond-then-reboot ordering is unchanged.

**`bb_http_req_get_header` (B1-527 follow-up).** `bb_err_t bb_http_req_get_header(bb_http_request_t *req, const char *name, char *out, size_t out_len)` (`components/bb_http/include/bb_http.h`) reads a single named request header into a caller-supplied bounded buffer. **Contract (both backends agree): `BB_OK` when the header is present** — value copied, NUL-terminated, truncated to `out_len-1` if longer than the buffer (a truncated value, e.g. a long User-Agent, is still usable) — **`BB_ERR_NOT_FOUND` when absent** (`out` set to `""`), **`BB_ERR_INVALID_ARG`** on null args or `out_len==0`. ESP-IDF impl (`platform/espidf/bb_http/bb_http.c`) unwraps the opaque handle to `httpd_req_t*` and calls `httpd_req_get_hdr_value_len` + `httpd_req_get_hdr_value_str`, mirroring the existing `bb_http_req_body_len`/`bb_http_req_recv` unwrap pattern (no new ESP-IDF type reaches the public header); both `ESP_OK` and `ESP_ERR_HTTPD_RESULT_TRUNC` map to `BB_OK` — only a genuinely absent header (or any other error) maps to `BB_ERR_NOT_FOUND`. Host stub (`platform/host/bb_http/bb_http_host.c`) is backed by a single-slot test hook, `bb_http_host_set_req_header(name, value)` (`bb_http_host.h`), and mirrors the same present-even-if-truncated → `BB_OK` contract, so the request-header-read path is host-testable like the rest of the capture harness. Arduino stub (`platform/arduino/bb_http/bb_http_arduino.cpp`) always returns `BB_ERR_NOT_FOUND` — Arduino's `bb_http` backend does not support request headers. `POST /api/reboot`'s `reboot_handler` calls it with `name="User-Agent"` and passes the resolved string into `bb_system_reboot_parse_body` (above) as the fallback when the JSON body's `detail` is absent/empty.

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

**Mode-aware `BB_PUB_WORKER_STACK`.** The worker stack auto-sizes: 6144 bytes when both TLS gates are OFF (plaintext only), 8192 bytes when `BB_MQTT_TLS_ENABLE` or `BB_HTTP_TLS_ENABLE` is ON. For the upstream esp-idf mbedTLS buffers, see the profile fragments in `sdkconfig/profiles/`.

**`bb_mqtt`** — portable MQTT client HAL. NVS namespace `bb_mqtt`; `/api/mqtt` PATCH/GET routes; TLS via `bb_tls_creds`. `bb_mqtt_publish(h, topic, payload, len, qos, retain)`. Telemetry config is **declarative / reboot-to-apply**: PATCH validates and persists to NVS only; exactly one enabled sink is wired at boot (registration == enabled); a reboot is required to apply changes (`POST /api/reboot`). PATCH returns `{"reboot_required":true}` on success; `GET /api/telemetry` exposes a top-level `pending_reboot` flag.

**`bb_pub`** — transport-agnostic telemetry core. Maintains a source registry and a fan-out set of `bb_pub_sink_t` sinks. `bb_pub_register_source(subtopic, sample_fn, ctx)` adds a source; `bb_pub_tick_once()` calls each source, injects shared `uptime_ms` (u64 ms via `bb_clock_now_ms64`), serializes the JSON once, then delivers it to every registered sink as `<prefix>/<hostname>/<subtopic>`. `bb_pub_set_sink` (back-compat, single-sink) replaces all sinks; `bb_pub_add_sink` appends to the fan-out set; `bb_pub_clear_sinks` empties it. A sink returning non-BB_OK is logged but does not abort delivery to other sinks. Periodic worker task registered at PRE_HTTP tier (`CONFIG_BB_PUB_AUTOREGISTER`, default y). Source cap: `CONFIG_BB_PUB_MAX_SOURCES` (default 16). Sink cap: `CONFIG_BB_PUB_MAX_SINKS` (default 4, range 1–8). Testing: `BB_PUB_TESTING` enables `bb_pub_test_reset()`. **Pause/resume:** `bb_pub_pause()` / `bb_pub_resume()` / `bb_pub_is_paused()` allow a consumer (e.g. an OTA pause hook) to quiesce publishing — while paused, `bb_pub_tick_once` is a cheap no-op (no sample_fn calls, no sink calls, no sockets/CPU). `bb_pub_pause()` uses a bounded-wait: it sets the paused flag (stopping new ticks) then waits on an in-publish condvar for any in-flight sink fan-out to complete, with a timeout of `CONFIG_BB_MQTT_NETWORK_TIMEOUT_MS` (default 30 s). This means pause() returns as soon as the in-flight publish finishes — not after the full TLS-write duration — preserving the no-concurrent-TLS guarantee without blocking OTA/update-check hooks for 14 s on slow mining boards. The tick lock (`s_tick_lock`) is released before the blocking `sk->publish()` / `buffer_replay` calls; only the CPU-bound sample + serialize phase runs under the lock. **Runtime config (NVS `bb_pub`):** `bb_pub_set_interval_ms(ms)` / `bb_pub_get_interval_ms()` — persist and live-apply the publish period (1 000–3 600 000 ms; ESP-IDF re-arms the timer immediately via `bb_pub_set_interval_apply_hook`). `bb_pub_set_enabled(bool)` / `bb_pub_is_enabled()` — persistent enable toggle (NVS key `enabled`, default on). Both gates are independent: tick publishes only when `enabled=true` AND not paused. The "publisher" `/api/telemetry` section is now read-write: GET reports `interval_ms` + `enabled`; PATCH sets and persists them with bounds validation. NVS re-arm requires on-hardware confirmation; host build updates in-RAM value only. **Payload extenders:** `bb_pub_register_payload_extender(fn, ctx)` registers a `bb_pub_payload_fn` callback invoked for each source's JSON object after `sample_fn` + `uptime_ms` injection but before serialize; extenders run in registration order and may add or alter any field (B1-270: source-in-body will be a built-in extender). Cap: `BB_PUB_MAX_PAYLOAD_EXTENDERS` (default 4, high-watermark warn at cap-1, `BB_ERR_NO_SPACE` on overflow). Not called when paused or disabled. Cleared by `bb_pub_test_reset()`.

**Store-and-forward ring buffer (`BB_PUB_BUFFER_ENABLE`).** The ring is a `bb_pool` FIFO with two backing modes selected by `CONFIG_BB_PUB_BUFFER_STATIC` (B1-489, replacing the B1-478 PR D permanent-BSS-only shape):
- **Default (`BB_PUB_BUFFER_STATIC=n`) — lazy HEAP-backed.** The pool owns a right-sized heap arena (`bb_pool_create_owned`, `BB_POOL_BACKING_HEAP`), created on first use (on-failure mode: first sink failure; always-on mode: once at init via `bb_pub_buffer_init_eager()`). In on-failure mode, after `CONFIG_BB_PUB_BUFFER_IDLE_FREE_TICKS` consecutive empty ticks post-drain the pool is destroyed (`bb_pool_destroy`, freeing the owned heap arena) — **~0 standing RAM cost when healthy**, recreated lazily on the next outage. A failed heap allocation (e.g. a fragmented no-PSRAM heap) is fail-soft: the ring is unavailable for that cycle and retried on the next outage (logged once per sustained failure streak, not every tick). In always-on mode the pool is created once and stays allocated for the process lifetime (no idle window to reclaim it) — heap-not-BSS is still the win over the static shape.
- **`BB_PUB_BUFFER_STATIC=y` — permanent static-BSS `bb_arena`.** The pre-B1-489 shape: the arena is sized once via `BB_PUB_RING_POOL_ARENA_BYTES` (512 B pool/arena struct headroom + 64 B per-entry overhead + `MAX_ENTRIES × ENTRY_MAX`) and is a **permanent BSS reservation — roughly ~8.5 KB at Kconfig defaults** (`512 + 16×64 + 16×449` — `ENTRY_MAX` = `TOPIC_MAX`(192) + 1 + `MAX_PAYLOAD_BYTES`(256) = 449), with no heap allocation ever and no idle-free reclaim (nothing to reclaim). Use this for a deterministic buffer under severe heap fragmentation.

Independent of the STATIC knob, `BB_PUB_BUFFER_ALWAYS` selects *when* the ring is populated:
- **On-failure mode (default, `BB_PUB_BUFFER_ALWAYS=n`):** when sink[0] fails, the serialized payload is captured. On the next tick where sink[0] succeeds, buffered entries are replayed oldest-first before live data. `captured_ms` is injected into any replayed entry that had a synced epoch at capture time (every buffered entry is by definition delayed, so injection is unconditional when `ts > 0`).
- **Always-on mode (`BB_PUB_BUFFER_ALWAYS=y`):** every sink[0] publish is enqueued into the ring and drained each tick (enqueue → drain), providing a uniform ordered queue. Sinks[1..] keep direct fan-out unchanged. `captured_ms` is injected ONLY when an entry is genuinely delayed across ticks: age `(now_epoch_ms − capture_epoch_ms) > 1.5 × interval_ms`. Fresh entries drained the same tick they were enqueued are below the threshold and carry no `captured_ms` field, keeping healthy-operation points clean. `last_publish_ok` in always-on mode reflects whether the ring drained fully (empty after replay), not individual direct-publish results. **Oversized-entry fallback:** if an entry's `topic_len + 1 + payload_len` exceeds the ring slot (`BB_PUB_BUFFER_ENTRY_MAX`), `buffer_capture` returns `false` and the always-on path publishes the entry directly (live) instead of dropping it — it is not buffered for replay, but the live data is preserved. This is the correct behavior for topics like `info` whose serialized payload can exceed the ring slot on no-PSRAM boards.

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
| `CONFIG_BB_PUB_WORKER_STACK` | 6144 (plain) / 8192 (TLS) | Worker task stack (bytes). Auto-sizes: 6144 when both `BB_MQTT_TLS_ENABLE` and `BB_HTTP_TLS_ENABLE` are OFF; 8192 when either is ON. Override in sdkconfig if needed. |
| `CONFIG_BB_PUB_MAX_SOURCES` | 16 | Source registry capacity. Each entry is a small struct (~24 B); rarely needs raising. |
| `CONFIG_BB_PUB_MAX_SINKS` | 4 | Sink array capacity (fan-out). Each entry is a function pointer + context pointer. |
| `CONFIG_BB_PUB_BUFFER_ENABLE` | n | Enable store-and-forward ring. Compiles in `bb_pool`/`bb_arena` dependency and capture/replay logic. Required for both on-failure and always-on modes. Default backing is lazy HEAP (see `BB_PUB_BUFFER_STATIC`), ~0 standing cost when healthy. |
| `CONFIG_BB_PUB_BUFFER_STATIC` | n | `depends on BB_PUB_BUFFER_ENABLE`. n (default) = lazy HEAP-backed ring, ~0 standing BSS when healthy, reclaimed after `BB_PUB_BUFFER_IDLE_FREE_TICKS` idle ticks. y = permanent static-BSS arena (~8.5 KB at Kconfig defaults), no heap allocation ever, no idle-free reclaim. |
| `CONFIG_BB_PUB_BUFFER_MAX_ENTRIES` | 16 | Ring capacity (entries). Full ring evicts oldest entry, increments `dropped` counter. |
| `CONFIG_BB_PUB_BUFFER_MAX_PAYLOAD_BYTES` | 256 | Max bytes per ring entry payload. Oversized payloads are rejected (not truncated). |
| `CONFIG_BB_PUB_BUFFER_ALWAYS` | n | Always-on ring mode (`depends on BB_PUB_BUFFER_ENABLE`). When y: all sink[0] traffic routes through the ring (enqueue→drain each tick) for a uniform ordered queue; pool is populated eagerly at boot and (in the default lazy-heap backing) stays allocated for the process lifetime — no idle window to reclaim it. When n (default): pool is populated lazily (created on first sink failure) and, in the default lazy-heap backing, freed again after idle ticks. Recommended on PSRAM boards for always-on. `captured_ms` is injected only when an entry is delayed beyond 1.5 × interval_ms in always-on mode; unconditionally (when `ts > 0`) in on-failure mode. |
| `CONFIG_BB_HTTP_CLIENT_TASK_STACK_SIZE` | 8192 | Referenced by `BB_HTTP_CLIENT_TASK_STACK` macro. Any task (including `bb_pub` worker) calling `bb_http_client_post` needs its stack ≥ this value. |
| `CONFIG_MQTT_TASK_STACK_SIZE` | 6144 | esp-mqtt internal task stack. Raise to 8192 when using TLS (MQTTS). |
| `CONFIG_MQTT_BUFFER_SIZE` | 1024 | esp-mqtt send/receive buffer (bytes). |
| `CONFIG_MQTT_OUTBOX_SIZE_BYTE` | 4096 | esp-mqtt QoS outbox (bytes, heap). |
| `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` | 16384 | TLS record input buffer. Drop to 4096 on no-PSRAM boards using HTTPS (AWS IoT accepts 4 KB records). |
| `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN` | 4096 | TLS record output buffer. |
| `CONFIG_MBEDTLS_DYNAMIC_BUFFER` | n | Dynamically allocate TLS buffers (frees them between records). Saves ~20 KB peak heap on no-PSRAM boards at the cost of more frequent allocs. |

**WROOM-32-class (no-PSRAM) guidance.** The classic ESP32 WROOM-32 has ~300 KB usable internal RAM. For plaintext-only deployments, skip mbedTLS entirely and leave `CONFIG_BB_PUB_WORKER_STACK` at its plaintext default of 6144 — do **not** drop it to 4096; that value caused the B1-349 stack-overflow crash and is below the Kconfig help's documented minimum for the serialize path. If TLS is required, set `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=4096`, enable `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`, and keep `CONFIG_BB_PUB_WORKER_STACK=8192`. The exclusive-sink arbiter guarantees only one telemetry sink (MQTTS or HTTPS) is ever active, so two concurrent TLS sinks cannot coexist by design. The remaining concurrent-TLS scenario to budget heap for is the active telemetry sink (MQTTS or HTTPS) running alongside the HTTPS update-check / OTA-pull client.

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

**Auto-register:** registered by `bb_telemetry_init` at order 5 via `bb_init` (`CONFIG_BB_TELEMETRY_AUTOREGISTER`, default y), alongside the `/api/telemetry` GET/PATCH routes.

## Releases

Tagging is manual: `git tag -a vX.Y.Z -m 'chore: vX.Y.Z tag' && git push origin vX.Y.Z`. The `release.yml` workflow waits for CI then publishes a GitHub release with auto-generated notes categorized by PR label (`.github/release.yml`). PR labels are auto-applied from conventional-commit prefixes; `new-component` PRs need that label set manually.

## Timer callback convention

esp_timer/bb_timer callbacks are **signal-only** — no `portMAX_DELAY` locks,
allocation, or IDF-subsystem calls in a timer callback. Two sanctioned deferral modes:

- **Light/slow-cadence housekeeping** → `bb_timer_deferred_*` (shared `bb_timer_disp`
  task; coalescing queue; caller-supplied `work_fn` runs off the esp_timer service task).
- **Heavy/hot/large-stack IO** (e.g. TLS, mbedTLS handshake) → `bb_timer_worker_*`
  (own task per timer; caller-sized `{stack, priority, core}` via `bb_timer_worker_cfg_t`).

Never hand-roll a timer→task pattern; never call `esp_timer_create` outside
`platform/espidf/bb_timer/bb_timer.c`.

## Workspace conventions

Workspace-level conventions (git, testing, docs) live in `/Users/jae/Projects/dangernoodle/CLAUDE.md`.

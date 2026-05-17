# ESP-IDF Bootstrap Component Library

Standalone ESP-IDF bootstrap component library. Reusable wifi provisioning, NVS, HTTP server, OTA, log streaming, display, and board abstraction components for any ESP-IDF firmware project.

## Public symbol prefix

All public C symbols use prefix `bb_`.

## Portability discipline

Public headers must not include `esp_*.h` or `freertos/*.h` outside `#ifdef ESP_PLATFORM`. This is enforced so non-ESP-IDF platform backends (e.g. Arduino) can coexist without breaking consumers.

The `bb_info` component demonstrates the portable-header pattern: its public extender callback takes a `bb_json_t` handle (from `bb_json`) rather than a backend-specific pointer, so consumer headers stay free of cJSON/ArduinoJson includes. Platform-specific logic lives under `platform/espidf/` and `platform/host/`.

For the Arduino backend specifically:
- Log format strings (`bb_log_*` calls) should be wrapped in `F()` to force them into PROGMEM to preserve SRAM on tiny platforms (2 KB on Uno). For now, breadboard's own logging is sparse enough that this is deferred — only enforce it when a real project hits SRAM pressure.
- Backend implementations are in `platform/arduino/<component>/` as `.cpp` files (not `.c`), since the Arduino framework and ecosystem (EEPROM, Serial, libraries) are C++ native.
- `bb_http` on Arduino batches the entire response (status + headers + body) into a single `client.write()` call due to unreliable `fastrprint()` flush behavior across many tiny writes. Request bodies are not supported to save SRAM on resource-constrained targets.

## API conventions

Public headers under `components/*/include/` follow these rules so that the same header compiles unchanged on ESP-IDF, Arduino, and host test backends.

- **Return `bb_err_t`**, never `esp_err_t` (or any other platform type). `bb_err_t` is a typedef for `esp_err_t` under ESP-IDF and `int` elsewhere — values pass through unchanged.
- **Cross-cutting types live in `bb_core`.** `bb_err_t`, `BB_OK`/`BB_ERR_*` macros, `bb_http_handle_t`, and `bb_http_request_t` are defined in `components/bb_core/include/bb_core.h`. Components REQUIRES `bb_core` for these types — never `bb_nv` for the error type or `bb_http` for the handle typedef alone.
- **Log via `bb_log_*`**, never `ESP_LOG*`. Portable impls live under `platform/<backend>/bb_log/`.
- **No `#ifdef ESP_PLATFORM` in public headers.** If a function needs an ESP-IDF type in its signature, wrap it behind an opaque `bb_*` handle (see `bb_http_request_t` wrapping `httpd_req_t *`).
- **No ESP-IDF headers** (`esp_err.h`, `esp_http_server.h`, `nvs_flash.h`, …) included transitively from public headers. Private `.c` files may include them freely.
- **Streaming JSON arrays:** For handlers emitting JSON arrays of N small objects, prefer `bb_http_resp_json_arr_*` (begin/emit/end) over building one large `bb_json_t` tree. On ESP-IDF, items stream in true chunked chunks so only one per-item subtree lives in memory; on Arduino/host it buffers for compatibility. Same external output; one API for both.

When extending an existing component: match the existing style even if older code predates these rules; a PR that flips 10 functions without migrating the rest is churn.

## Logging

Use `bb_log_{e,w,i,d,v}(tag, fmt, ...)` macros for all breadboard component code. On ESP-IDF these expand to `ESP_LOG{E,W,I,D,V}`; on host they map to `fprintf` (debug/verbose compile out to keep test output clean).

## Layout

- Components under `components/<name>/` with public headers.
- Platform-specific implementations under `platform/espidf/` and `platform/arduino/`.
- Component names are prefixed `bb_`.

## Header visibility and component coupling

These rules govern which headers go where and how components depend on each other. Violations show up as `_port.h`/`_internal.h` files reaching across component boundaries, sibling tests grepping into another component's implementation directory, and `REQUIRES` lists that grow because nobody pruned them.

### Public vs private headers

- `components/<name>/include/` is the **public** surface — anything in there is reachable by every consumer that REQUIRES the component. Only headers intended for cross-component use belong here.
- Platform-port contracts, test-injection hooks, vtables, and anything ending in `_port.h` / `_internal.h` / `_priv.h` are **private**. Put them next to the implementation (`components/<name>/<name>_internal.h` or `components/<name>/private/`), not in `include/`.
- Platform impls (`platform/<backend>/<name>/`) include the private header via a relative path; consumers cannot reach it.

### REQUIRES vs PRIV_REQUIRES

- If your **public header** includes a header from another component, that component goes in `REQUIRES` — its types appear in your API.
- If your `.c` / `.cpp` / private headers include a header from another component but your public headers don't, that component goes in `PRIV_REQUIRES` — it's an implementation detail.
- Default to `PRIV_REQUIRES`. Adding to `REQUIRES` is a commitment to expose another component's surface transitively.
- Test code is not part of the component's REQUIRES contract; test-only deps go in the `test_*` env's `lib_deps`, not the component CMakeLists.

### Tests must not reach into other components

- A test file may `#include` any public header (`components/<name>/include/<name>.h`).
- A test file MUST NOT `#include "../../components/X/src/...h"` or `#include "../../components/X/X_internal.h"` to call private functions. If a test needs a hook (mock allocator, state reset, etc.), the owning component exposes it through a dedicated `<name>_test.h` header guarded by `BB_<NAME>_TESTING`, and the test includes that header by name.

### Includes at file top

`#include` lives at the top of the file with the other includes. Mid-file `#include` directives hide dependencies from grep and from anyone scanning the include block for impact; conditional gating belongs around the *use* of the symbol, not the include itself.

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
`BB_REGISTRY_REGISTER_N` accepts an integer `order` as the third argument. The regular-tier walker sorts entries by `.order` ascending before invoking inits — lower numbers run first. Ties preserve registration (insertion) order, i.e. the sort is stable. `BB_REGISTRY_REGISTER` is shorthand for `BB_REGISTRY_REGISTER_N(..., 0)`. Example: `bb_event_routes` registers at order 0; `bb_update_check` registers at order 4 so `bb_event_routes_init` (which sets `s_cfg.initialized = true`) has already run before `bb_update_check` calls `bb_event_routes_attach`.

All three tiers require the matching CMake helper (in `cmake/bb_registry.cmake`) to prevent garbage-collection under PlatformIO:

```cmake
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
bb_registry_force_register(${COMPONENT_LIB} bb_<name>)           # regular
bb_registry_force_register_early(${COMPONENT_LIB} bb_<name>)     # early
bb_registry_force_register_pre_http(${COMPONENT_LIB} bb_<name>)  # pre_http
```

Today this pattern owns — regular: `bb_ota_pull`, `bb_ota_push`, `bb_info`, `bb_log` (routes), `bb_board`, `bb_manifest`, `bb_ota_validator`, `bb_wifi` (routes), `bb_system` (routes), `bb_openapi`, `bb_mdns`. Early: `bb_log_stream`, `bb_nv_flash`, `bb_nv_config`, `bb_wifi` (STA init via `CONFIG_BB_WIFI_AUTOREGISTER`). PRE_HTTP: none yet (tier is available for consumers). HTTP server autostart is gated on `CONFIG_BB_HTTP_AUTOSTART` (default y); disable it if CORS or OpenAPI config must precede server start. Socket reservation for non-httpd usage (stratum TCP, mDNS UDP, transient outbound) is tunable via `CONFIG_BB_HTTP_LWIP_RESERVE` (default 3, range 1–6) — lower it to give httpd more headroom, raise it to preserve slots for outbound work.

The `bb_nv_config` component persists device configuration to NVS: WiFi credentials, hostname (32 chars max, RFC1123 charset), and DHCP/mDNS feature flags. Hostname is available via `bb_nv_config_hostname()` and settable via `bb_nv_config_set_hostname()` with validation for leading/trailing hyphens and non-alphanumeric characters.

`bb_wifi_autoinit` (EARLY tier) reads the hostname from `bb_nv_config_hostname()` and applies it before connecting so DHCP/mDNS see the right name on first packet; on validated builds it retries the STA connect indefinitely (30 s backoff, `CONFIG_BB_WIFI_RETRY_FOREVER_WHEN_VALIDATED`, default y) so a network outage doesn't knock the device into AP mode; errors are swallowed so the EARLY walker continues for provisioning flows.

## Event bus (bb_event, bb_event_ring, bb_event_routes)

`bb_event` is a portable callback-list publish/subscribe event bus. On ESP-IDF, subscribers receive events via a FreeRTOS dispatcher task; on Arduino, the app pumps events from `loop()` via `bb_event_pump()`. `bb_event_ring` is a sibling component providing a circular buffer with replay-on-subscribe — designed for fan-out scenarios (SSE/WebSocket subscribers, persistent event history). Both use the same `bb_event_t` opaque handle and dispatch model; `bb_event_ring` layers replay semantics on top of `bb_event` internally.

`bb_event_routes` exposes `GET /api/events` as a Server-Sent Events stream. Producers call `bb_event_routes_attach("topic.name")` to surface a topic on the stream; the route fans out to multiple concurrent clients, each with its own replay-on-connect via `bb_event_ring`. The payload contract is **caller posts valid UTF-8 JSON** — the route emits it raw in the SSE `data:` field. Arduino targets ship a 503 stub today (CC3000 RAM constraints); ESP-IDF is the production path.

**`_ex` variants and the retained flag.** Both `bb_event_ring_attach` and `bb_event_routes_attach` have `_ex` variants that accept a `bool retained` parameter:
- `bb_event_ring_attach_ex(topic, capacity, max_entry, retained, out)` — preferred form; `bb_event_ring_attach` is `retained=false`.
- `bb_event_routes_attach_ex(topic_name, retained)` — preferred form; `bb_event_routes_attach` is `retained=false`.

Set `retained=true` for **state topics** (topics that publish current state, not discrete events). The flag documents intent and reserves API surface for future drain semantics. The practical effect today: callers that set `retained=true` MUST also publish an initial snapshot at component init so the ring is non-empty from T=0 — this ensures SSE clients connecting before the first periodic post receive the last known value rather than empty state.

**Auto-attach convention.** A component that emits a `bb_event` topic owns its own attach to `bb_event_routes`, gated by `CONFIG_BB_<NAME>_AUTO_ATTACH` (default y, `depends on BB_EVENT_ROUTES_AUTOREGISTER`). Consumers enabling both components get the topic on `/api/events` without an explicit `bb_event_routes_attach()` call. Disable the flag if you want the topic available for internal subscribers only. Today `bb_update_check` follows this pattern (`update.available`) using `bb_event_routes_attach_ex(..., true)`; future emitters (`ota.progress`, `mining.share.*`) will follow the same shape.

**Topic discovery and diagnostics.** `GET /api/diag/events` lists every topic currently attached to `/api/events` with ring-buffer diagnostics: `ring_capacity`, `ring_count` (0 → no replay possible), `last_id`, `last_post_us`, and `last_size`. Top-level fields `max_clients` and `active_clients` show the configured concurrency cap and how many SSE connections are live. This is the first step when SSE shows `: connected` but no replay data — confirm the topic is attached and `ring_count > 0` before investigating the producer.

**Platform split (ESP-IDF components).** `bb_event_ring` and `bb_event_routes` are portable components with no ESP-IDF dependencies in their `components/` CMakeLists. ESP-IDF platform implementations live in standalone components under `platform/espidf/`:
- `platform/espidf/bb_event_ring_espidf/` — provides `bb_event_ring_now_us()` backed by `esp_timer_get_time()`. REQUIRES `bb_event_ring esp_timer`.
- `platform/espidf/bb_event_routes_espidf/` — registers `/api/events` + `/api/diag/events` HTTP routes. REQUIRES `bb_event_routes` and all ESP-only deps (`bb_json`, `esp_http_server`).

ESP-IDF consumers must add both directories to `EXTRA_COMPONENT_DIRS` and include `bb_event_ring_espidf` and `bb_event_routes_espidf` in their component's `REQUIRES` list (or the app's `idf_component_register` REQUIRES). The portable `bb_event_ring` and `bb_event_routes` components declare no `esp_timer`, `esp_http_server`, or `bb_json` dependencies.

**Heap budgets and Kconfig defaults.** Each SSE client consumes heap for its payload buffer, queue entries, and synchronization primitives. At defaults `BB_EVENT_ROUTES_MAX_CLIENTS=2` and `BB_EVENT_ROUTES_QUEUE_DEPTH=8`, each client uses ~2.7 KB (8×256 B payload buf + 8×24 B queue entry overhead + ~88 B mutex). These defaults are conservative for tight-heap boards without PSRAM (ESP32-S3, ~30 KB heap budget reserved for idle TLS handshake). Consumers with more heap can override via `sdkconfig`. See components/bb_event_routes/Kconfig for rationale.

## Update check (bb_update_check, bb_release_manifest, bb_http_client)

`bb_update_check` periodically polls a release-manifest URL, compares the returned version against `bb_system_get_version()`, and on a state change posts the `update.available` `bb_event` topic and updates the mDNS TXT key `update=<value>` (`unknown` pre-check, `none` up to date, `<tag>` when newer is available). **Initial snapshot publishing.** `bb_update_check_init` does NOT post an initial snapshot; callers must explicitly call `bb_update_check_publish_initial()` after attaching a ring/routes to the topic (via `bb_event_routes_attach_ex(..., true)` with `retained=true`, or `bb_event_ring_attach_ex(..., true)`). This ensures SSE clients connecting before the first periodic check (up to `CONFIG_BB_UPDATE_CHECK_INTERVAL_S` seconds) replay the last known state rather than seeing empty state. The initial snapshot has `available=false`, `current=<running version>`, `latest=""`, `download_url=""`, `last_check_ok=false`. The ESP-IDF platform implementation (`bb_update_check_register_init`) calls `bb_update_check_publish_initial()` automatically after attaching under `CONFIG_BB_UPDATE_CHECK_AUTO_ATTACH`. Consumers call `bb_update_check_set_releases_url(url)` and optionally `bb_update_check_set_parser(fn)` to swap the manifest parser. The default parser is `bb_release_manifest_parse_github`; consumers using GitLab, Jenkins, S3, or a private artifact server supply their own `bb_release_manifest_parse_fn`. Fetches go through `bb_http_client` — the same portable outbound HTTP wrapper used by both `bb_update_check` and `bb_ota_pull` for release manifest fetches. The component auto-registers `GET /api/update/status` and is ESP-IDF only; Arduino targets stub every setter to `BB_ERR_UNSUPPORTED`.

**Firmware board name.** `bb_update_check_set_firmware_board(board)` overrides the asset name prefix used when matching release assets. The default is `"firmware"` (looks for `firmware.bin`). Pass the board prefix without `.bin` (e.g. `"taipanminer-tdongle-s3"` matches `taipanminer-tdongle-s3.bin`). Pass `NULL` or `""` to revert to the default. Mirrors `bb_ota_pull_set_firmware_board` so consumers can pass the same `"taipanminer-" FIRMWARE_BOARD` string to both components. Returns `BB_ERR_INVALID_STATE` before init, `BB_ERR_INVALID_ARG` if the string exceeds 63 chars.

**Streaming fetch path.** The default GitHub parser (`bb_release_manifest_parse_github`) is invoked via the streaming API: `bb_http_client_get_stream` feeds 2 KB chunks into `bb_release_manifest_parse_github_stream_feed` as bytes arrive over the socket. Only the parser state (~400 B on stack) and the two extracted strings (`tag_name`, `browser_download_url`) live in memory — no per-fetch body buffer is allocated. Custom parsers registered via `bb_update_check_set_parser` fall back to a local 16 KB heap allocation per fetch (freed immediately after the call). `bb_ota_pull` uses `bb_update_check_get_status()` to read the cached check result rather than performing its own manifest fetch. `ota_fetch_manifest` (in `bb_ota_pull.c`, outside `#ifdef ESP_PLATFORM`) is still compiled for the `BB_OTA_PULL_TESTING` hook; it is no longer called on device.

**`bb_update_check` is the single source of truth for manifest checks.** `bb_update_check_get_status(bb_update_check_status_t *out)` copies the cached status snapshot under the internal pthread mutex. Callers do not hold any lock. `POST /api/ota/update` reads this to extract `latest` and `download_url` for the OTA worker; returns 503 if `last_check_ok == false` (no recent successful check), 409 if `available == false`. `GET /api/ota/check` is a thin back-compat wrapper that calls `bb_update_check_now()` and returns `{"status":"checking"}` (200) immediately; callers poll `GET /api/update/status` for results.

The streaming parser (`bb_release_manifest_parse_github_stream_{begin,feed,end}`) is a resumable byte-at-a-time state machine in `bb_release_manifest_github_stream.c`. It handles keys and values split across chunk boundaries, nested objects in the assets array, and the same matching rule as the buffered parser: `assets[].name == "<board_fallback>.bin"` → take that asset's `browser_download_url`.

**Pause/resume hooks.** `bb_update_check_set_hooks(pause_fn, resume_fn)` lets consumers bracket each manifest fetch with optional callbacks. `pause_fn` is called just before `bb_http_client_get_stream`; `resume_fn` immediately after — on both success and failure paths. This mirrors `bb_ota_pull_set_hooks` and exists for the same reason: on tight-heap dual-core boards (tdongle-s3, bitaxe), the mbedTLS handshake transient (~20–30 KB) on top of an active mining workload causes OOMs. Consumers suspend the ASIC/mining task in the pause hook and resume it in the resume hook. Either argument may be NULL to disable that side.

**Task stack budget.** Any task that calls `bb_http_client_get*` must allocate at least `BB_HTTP_CLIENT_TASK_STACK` bytes (8 KiB default, `CONFIG_BB_HTTP_CLIENT_TASK_STACK_SIZE`). The mbedTLS handshake + cert-bundle parse path needs 5–8 KiB; a smaller task stack overflows into adjacent heap blocks and corrupts heap metadata, surfacing as an unrelated assertion in the next `calloc`. Both `bb_update_check` and `bb_ota_pull` reference the macro so the budget stays consistent.

## Provisioning UI

The `bb_prov` component manages the provisioning state machine and HTTP `/save` handler. Callers MUST supply at least one asset with `path="/"` to `bb_prov_start`. For bare-minimum bringup, add `REQUIRES bb_prov_default_form` and pass `&bb_prov_default_form_asset`. Custom UIs pass their own asset array instead. `POST /save` returns `204 No Content`; the caller's form JS is responsible for post-submit UX.

`bb_prov_start(assets, n, extra)` owns the full prov-mode route graph: it registers `/save`, assets, registry routes (e.g. `/api/version`, `/api/scan`, `/api/reboot`), an optional consumer `extra` callback, then the captive-portal `/*` GET wildcard — in that exact order so specific handlers always win first-match. Pass `NULL` for `extra` when the UI only needs the built-ins. Use `extra` for advanced UIs that need dynamic endpoints (e.g. live diagnostics, pool-test buttons); register them on `server` and bb_prov will sequence them correctly. Additional form fields (pool/wallet/worker/etc.) stay on the `/save` body and are parsed via `bb_prov_set_save_callback`.

## Display

LVGL is initialized inside `bb_display_init` via `esp_lvgl_port`. The consumer's `sdkconfig` governs LVGL font availability (`CONFIG_LV_FONT_MONTSERRAT_*`) and color depth (must be 16-bit / RGB565); breadboard does not ship pre-defined dashboard layouts. Every call into LVGL from application code (including `lv_timer` callbacks not running on the LVGL task) must be wrapped in `bb_display_lock` / `bb_display_unlock`. The `bb_display` component is currently ESP-IDF-only.

## bb_hw

Board pin/peripheral headers are consumer-supplied. Each firmware must provide its own board header file. In your build, set `-DBB_HW_BOARD_HEADER="<name>.h"` and ensure that header is on the include path (via CMake include dirs or PlatformIO `build_flags`). `bb_hw.h` requires this define and will error if not provided.

Example (PlatformIO):
```
build_flags =
    -I${PROJECT_DIR}/board
    -DBB_HW_BOARD_HEADER=\"my_board.h\"
```

Examples in this repo (elecrow-p4-hmi7, esp32-wroom-32) own their own board headers under `examples/<name>/board/`. bb_hw component provides no bundled board definitions.

## bb_diag

`bb_diag` registers diagnostic HTTP routes under `/api/diag/`: panic log, coredump, heap stats, FreeRTOS task list, and abnormal-reset counter. `GET /api/diag/sockets` dumps LWIP TCP state distribution (per-state counts + per-PCB detail) for diagnosing socket exhaustion.

## Releases

Tagging is manual: `git tag -a vX.Y.Z -m 'chore: vX.Y.Z tag' && git push origin vX.Y.Z`. The `release.yml` workflow waits for CI then publishes a GitHub release with auto-generated notes categorized by PR label (`.github/release.yml`). PR labels are auto-applied from conventional-commit prefixes; `new-component` PRs need that label set manually.

## Workspace conventions

Workspace-level conventions (git, testing, docs) live in `/Users/jae/Projects/dangernoodle/CLAUDE.md`.

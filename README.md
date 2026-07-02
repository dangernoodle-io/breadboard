# breadboard

[![Build](https://github.com/dangernoodle-io/breadboard/actions/workflows/build.yml/badge.svg)](https://github.com/dangernoodle-io/breadboard/actions/workflows/build.yml)
[![Coverage Status](https://coveralls.io/repos/github/dangernoodle-io/breadboard/badge.svg?branch=main)](https://coveralls.io/github/dangernoodle-io/breadboard?branch=main)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Reusable components for embedded systems: wifi provisioning, NVS storage, HTTP server, OTA, log streaming, and hardware abstraction. Supports ESP-IDF (production) and Arduino (experimental).

> **Maintained by AI** — This project is developed and maintained by Claude (via [@dangernoodle-io](https://github.com/dangernoodle-io)).
> If you find a bug or have a feature request, please [open an issue](https://github.com/dangernoodle-io/breadboard/issues) with examples so it can be addressed.

**Status:** Pre-release (ESP-IDF APIs stable; Arduino backend beta).

## Components

| Component | Purpose | Platforms |
|-----------|---------|-----------|
| `bb_hw` | Consumer-supplied board pin/peripheral header resolved at compile time via `-DBB_HW_BOARD_HEADER="<name>.h"` | ESP-IDF |
| `bb_display` | MIPI-DSI panel init (EK79007) with LVGL via `esp_lvgl_port`; consumer holds `bb_display_lock` for all LVGL calls. Exposes `bb_display_screen` / `bb_display_lock` / `bb_display_unlock` for direct LVGL access. | ESP-IDF |
| `bb_display_spi_common` | Shared SPI helpers for SPI-based display drivers: `bb_display_spi_init_bus()`, `bb_display_blit_spi()`, `bb_display_clear_spi()`. Shared bounce buffer used by ili9341 and st77xx. `BB_DISPLAY_AUTOREGISTER` macro replaces per-driver constructor boilerplate. | ESP-IDF |
| `bb_event` | Generic app-level event bus on a portable callback list. Multi-subscriber publish/subscribe with queued dispatch; ESP-IDF uses a FreeRTOS dispatcher task, Arduino requires `bb_event_pump()` from `loop()`. | ESP-IDF, Arduino |
| `bb_event_ring` | Circular buffer variant of `bb_event` with replay-on-subscribe — for SSE/WebSocket fan-out and event history. | ESP-IDF, Arduino |
| `bb_event_routes` | Surfaces attached `bb_event` topics on `GET /api/events` as Server-Sent Events. Multi-client with per-topic replay; payload contract is "producer posts valid JSON, route emits raw". | ESP-IDF (Arduino: 503 stub) |
| `bb_http_client` | Portable outbound HTTP GET wrapper used by `bb_update_check` (and migrating `bb_ota_pull`) so consumers don't reach into `esp_http_client` directly. Host build ships a mock-response hook for tests | ESP-IDF, host (Arduino: stub) |
| `bb_release_manifest` | Provider-pluggable release-manifest parser: typedef for a `bb_release_manifest_parse_fn` plus built-in `bb_release_manifest_parse_github`. Consumers wanting GitLab/Jenkins/S3 register their own parser | ESP-IDF, host, Arduino |
| `bb_update_check` | Periodic poll of a configurable release manifest URL; posts `update.available` `bb_event` topic and `update=<value>` mDNS TXT on state changes. Auto-registers `GET /api/update/status` | ESP-IDF (Arduino: stub) |
| `bb_json` | Portable JSON builder + minimal parser; cJSON backend on ESP-IDF/host, ArduinoJson backend on Arduino. Opaque `bb_json_t` handle — no backend headers leak into public API. | ESP-IDF, Arduino |
| `bb_http` | HTTP server wrapper with portable route registration API; optional `bb_route_t` descriptors carry OpenAPI metadata for `bb_openapi` consumption; Arduino backend routes/handlers with fixed-buffer response batching | ESP-IDF, Arduino |
| `bb_log` | Ring-buffered log capture, runtime tag-level control, and `bb_log_{e,w,i,d,v}` macros for platform-abstract logging. Optional routes module (`CONFIG_BB_LOG_ROUTES`, default-on) adds log-level GET/POST; structured log stream served at `GET /api/events?topic=log` (`CONFIG_BB_LOG_EVENT_AUTO_ATTACH`, default-on) | ESP-IDF, Arduino |
| `bb_nv` | Typed NVS accessors plus generic `bb_nv_*` key/value helpers with caller-supplied namespace | ESP-IDF, Arduino |
| `bb_ota_pull` | HTTP releases-feed poller with cJSON parse and A/B rollback | ESP-IDF |
| `bb_ota_push` | HTTP firmware upload handler | ESP-IDF |
| `bb_ota_validator` | Owns the full OTA rollback state machine: boot-time pending detection, rollback-safety preflight, `bb_ota_mark_valid(reason)` signal API, POST `/api/ota/mark-valid` | ESP-IDF (portable stubs on non-ESP) |
| `bb_board` | Runtime sysinfo (chip model, cores, flash, heap, OTA state) and GET `/api/board` | ESP-IDF |
| `bb_clock` | Portable millisecond timestamp (`bb_clock_now_ms()`) in `bb_core/include/bb_clock.h`; no platform headers in consumers. | ESP-IDF, Arduino, host |
| `bb_info` | Composite GET `/api/info` merging sysinfo + wifi + consumer-registered extender callbacks | ESP-IDF |
| `bb_mdns` | mDNS service registration (registry auto-init via `CONFIG_BB_MDNS_AUTOREGISTER`); setters for hostname, instance, service-type are caller-driven | ESP-IDF |
| `bb_openapi` | Opt-in OpenAPI 3.1 spec emitter; walks the `bb_http` route descriptor registry to publish `GET /api/openapi.json` via registry auto-registration; same emitter drives build-time codegen via `host_tools/emit_openapi` | ESP-IDF, host |
| `bb_manifest` | Opt-in device manifest endpoint; consumer registers NVS keyspaces and mDNS TXT keys to expose `GET /api/manifest` with keyspace/enum descriptors for external tools (custom flashers, fleet provisioners) | ESP-IDF |
| `bb_prov` | Provisioning state machine (SoftAP + captive-portal + HTTP `/save` handler) | ESP-IDF |
| `bb_prov_default_form` | Opt-in default WiFi setup form asset (`bb_prov_default_form_get()`) for bare-minimum `bb_prov` bringup | ESP-IDF |
| `bb_init` | Handler-lifecycle registry: opt-in components self-register an init fn via `BB_INIT_REGISTER`; the app calls `bb_init_init(server)` once after `bb_http_server_start` to invoke them all | ESP-IDF, host |
| `bb_system` | Device restart and system info; optional routes module (`CONFIG_BB_SYSTEM_ROUTES_AUTOREGISTER`, default-on) adds GET `/api/version`, GET `/api/ping`, POST `/api/reboot` | ESP-IDF |
| `bb_arena_tls` | Boot-reserved contiguous mbedTLS handshake arena (`CONFIG_BB_ARENA_TLS_BYTES`), built on the generic `bb_arena` primitive; installs a custom `mbedtls_platform_set_calloc_free` allocator that tries the static arena first and falls back to the internal-heap facade (`bb_calloc_internal`/`bb_mem_free`) — prevents mid-uptime heap fragmentation on no-PSRAM boards. No-op when `CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC` is unset. | ESP-IDF, host |
| `bb_arena` | Generic, multi-instance contiguous-buffer bump allocator (caller-supplied or `bb_mem`-backed) | ESP-IDF, host |
| `bb_wifi` | STA init, async scan, auto-reconnect, diagnostics and GET `/api/wifi`; optional routes module (`CONFIG_BB_WIFI_ROUTES_AUTOREGISTER`, default-on) gates HTTP routes | ESP-IDF |

## Use in an ESP-IDF project

Append the `components/` directory to your project's top-level `CMakeLists.txt`:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS "<path-to>/breadboard/components")
```

Pick individual components in your app's `idf_component_register(... REQUIRES ...)` — you only pay build cost for what you use.

To run breadboard's conventions linter against your consumer project, add one line to your `CMakeLists.txt` (after `include()`):

```cmake
include("<path-to>/breadboard/cmake/bbtool.cmake")
bb_lint()  # opt-in: cmake --build build --target bb_lint
```

`bb_lint()` creates a non-default target; it never runs on a normal build. Invoke it explicitly: `cmake --build build --target bb_lint`. Optional args: `ROOT <dir>` (default `CMAKE_SOURCE_DIR`), `PROFILE <consumer|library>` (default `consumer`), `TARGET <name>` (default `bb_lint`).

Components that register HTTP handlers (`bb_ota_pull`, `bb_ota_push`, `bb_info`, `bb_board`, `bb_manifest`, `bb_ota_validator`, `bb_openapi`, plus optional routes modules in `bb_log`, `bb_wifi`, and `bb_system`) self-register through `bb_init`. After `bb_http_server_start`, call `bb_init_init(server)` once and every linked component's routes get wired up — no per-component `register_handler` calls in your `app_main`. Components still have to be listed in your CMake `REQUIRES` so the linker pulls their archives and the constructors fire.

## Kconfig flags

Auto-registration is opt-out. Each registry-using component exposes a Kconfig flag (default-on); flip to `n` in your `sdkconfig.defaults` to drop the route handler without losing the component's public C API.

| Flag | Effect when `=n` |
|------|------------------|
| `CONFIG_BB_OTA_PULL_AUTOREGISTER` | OTA pull HTTP routes not registered; `bb_ota_pull` C API still callable |
| `CONFIG_BB_OTA_PUSH_AUTOREGISTER` | OTA push HTTP route not registered |
| `CONFIG_BB_INFO_AUTOREGISTER` | `/api/info` not registered; `bb_info_register_extender` still works |
| `CONFIG_BB_LOG_ROUTES` | `bb_log` routes module dropped entirely — `bb_http`/`bb_json`/`bb_init` no longer in `bb_log`'s PRIV_REQUIRES, useful for headless-logging consumers |
| `CONFIG_BB_LOG_STREAM_AUTOREGISTER` | `bb_log_stream_init` not auto-called via `bb_init_init_early()`; caller must invoke manually. Independent of `CONFIG_BB_LOG_ROUTES` (stream capture vs. HTTP routes) |
| `CONFIG_BB_NV_FLASH_AUTOREGISTER` | `bb_nv_flash_init` not auto-called via `bb_init_init_early()`; caller must invoke manually |
| `CONFIG_BB_NV_CONFIG_AUTOREGISTER` | `bb_nv_config_init` not auto-called via `bb_init_init_early()`; caller must invoke manually. Useful for consumers with custom NVS namespaces who want partition init but not typed config preload |
| `CONFIG_BB_BOARD_AUTOREGISTER` | `/api/board` not registered; `bb_board` accessor C API still callable |
| `CONFIG_BB_MANIFEST_AUTOREGISTER` | `/api/manifest` not registered; `bb_manifest_register_nv` and `bb_manifest_register_mdns` still work |
| `CONFIG_BB_OTA_VALIDATOR_AUTOREGISTER` | `POST /api/ota/mark-valid` not registered; `bb_ota_mark_valid` and `bb_ota_is_pending` still work |
| `CONFIG_BB_WIFI_ROUTES_AUTOREGISTER` | `/api/wifi` and `/api/scan` not registered; wifi driver init APIs still work |
| `CONFIG_BB_SYSTEM_ROUTES_AUTOREGISTER` | System routes module dropped entirely; `bb_http`/`bb_json`/`bb_init`/`esp_timer` no longer in `bb_system`'s PRIV_REQUIRES |
| `CONFIG_BB_OPENAPI_AUTOREGISTER` | `/api/openapi.json` not registered; `bb_openapi_emit` and `bb_openapi_set_meta` still work |
| `CONFIG_BB_MDNS_AUTOREGISTER` | `bb_mdns_init` not auto-called via registry; caller must invoke it manually (setters still available) |
| `CONFIG_BB_OTA_PUSH_MAX_SIZE` | Body size cap for `POST /api/ota/push` (default 4 MB); requests over the limit return 413 |
| `CONFIG_BB_HTTP_ROUTE_REGISTRY_CAP` | Max registered HTTP routes (default 64); `bb_http_register_*` returns `BB_ERR_NO_SPACE` when full |
| `CONFIG_BB_LOG_TAG_REGISTRY_MAX` | Max log tag entries (default 24) |
| `CONFIG_BB_MDNS_TXT_PENDING_MAX` | Max pending mDNS TXT updates (default 4) |
| `CONFIG_BB_UPDATE_CHECK_CUSTOM_PARSER_BUF_BYTES` | Heap buffer for custom manifest parsers (default 8192 bytes) |

Tagged source archives will be published on the [releases page](https://github.com/dangernoodle-io/breadboard/releases) once the API stabilizes.

## Provisioning UI

The `bb_prov` component manages the provisioning state machine and HTTP `/save` handler. Consumers provide a `prov_ui_routes_fn` callback to `bb_prov_start` to register `GET /` and any static assets (favicon, css, logo). `POST /save` returns `204 No Content`; the caller's form JS is responsible for post-submit UX.

## Portability

Public headers guard `esp_*.h` and `freertos/*.h` behind `#ifdef ESP_PLATFORM` so non-ESP backends (e.g. Arduino) can coexist without breaking consumers. Components are designed portably even when only one backend is fully implemented. Arduino has validated backends for `bb_log`, `bb_nv`, `bb_http`, and the `bb_system_*` helpers driving `/api/version` and `/api/reboot`. `bb_ota_validator` exposes a portable strategy-struct API, with stubs on non-ESP platforms. Other components are currently ESP-IDF-only; progressive un-fencing is planned.

**Component authors:** See the API conventions section in [CLAUDE.md](CLAUDE.md) for portability rules.

## Host tests in downstream projects

Downstream projects that host-test code linking against `bb_*` components can opt into a Python scaffold script that automates the wiring of includes, source files, and dependencies. Rather than manually managing include paths and build filters for each component, declare your dependencies once:

```ini
[env:native]
extra_scripts = pre:.breadboard/scripts/native_scaffold.py
custom_bb_components = bb_log bb_nv bb_json
```

The scaffold resolves absolute paths to breadboard sources, handles cJSON lib_dep automatically if `bb_json` is listed, and skips de-duplication if a path is already present. Unknown component names cause the build to fail with a clear error. See `COMPONENT_MAP` in the script for the full list of supported components.

## Development

```bash
make check              # cppcheck static analysis
make coverage           # host unit tests + gcovr → Coveralls-format JSON
make smoke-elecrow-p4-hmi7     # build ESP-IDF P4 example (LVGL + EK79007 panel)
make smoke-esp32-wroom-32      # build classic ESP32-D0 example (headless)
make smoke-arduino-uno-cc3000  # build Arduino Uno + CC3000 example
make smoke                     # build all examples
```

Host tests under `test/test_host/` cover `bb_log` (macro expansion and ring buffer drain), `bb_nv` (typed and generic), `bb_prov`, `http_utils`, `bb_ota_pull`, `bb_ota_push`, `bb_ota_validator`, `bb_http` (route registry), `bb_openapi` (emitter, including OOM cleanup paths via `bb_json_host_force_alloc_fail_after`), `bb_json` (round-trip build/serialize/parse for all value types, nested trees, arrays, edge cases), `bb_tls_creds`, `bb_mqtt` (+ `/api/mqtt` routes), `bb_http_client` (POST + mutual-TLS), `bb_pub` (source registry, tick cycle, sink routing), `bb_sink_mqtt`, `bb_sink_http` (+ `/api/httppub`), and the telemetry satellites (`bb_pub_fan`, `bb_pub_power`, `bb_pub_thermal`, `bb_pub_info`, `bb_pub_wifi`). The `bb_hw`, `bb_display`, and hardware-coupled components have no host coverage.

**Telemetry publisher.** `bb_pub` + `bb_sink_mqtt` provide a transport-agnostic telemetry pipeline that publishes `metrics/<hostname>/<subtopic>` JSON (each cycle stamped with a shared `ts`). Add `bb_pub_fan`, `bb_pub_power`, `bb_pub_thermal`, `bb_pub_info`, and/or `bb_pub_wifi` to REQUIRES to auto-register per-HAL sources; each returns false (skip) when its source is absent, so builds without specific hardware emit nothing for that subtopic. `bb_mqtt` is the MQTT client (broker config via `/api/mqtt`, TLS via `bb_tls_creds`); `bb_sink_http` is an alternative HTTP-POST sink to a user-configurable endpoint (e.g. AWS IoT Core) over mutual TLS.

See `examples/arduino-uno-cc3000/README.md` for Arduino development setup (Homebrew toolchain on macOS, stock PIO toolchain on Linux).

## License

See [LICENSE](LICENSE) (MIT).

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
| `bb_hw` | Compile-time pin/peripheral map header selection via `FIRMWARE_BOARD_*` define | ESP-IDF |
| `bb_display` | MIPI-DSI panel init (EK79007) with LVGL via `esp_lvgl_port`; consumer holds `bb_display_lock` for all LVGL calls. Exposes `bb_display_screen` / `bb_display_lock` / `bb_display_unlock` for direct LVGL access. | ESP-IDF |
| `bb_json` | Portable JSON builder + minimal parser; cJSON backend on ESP-IDF/host, ArduinoJson backend on Arduino. Opaque `bb_json_t` handle — no backend headers leak into public API. | ESP-IDF, Arduino |
| `bb_http` | HTTP server wrapper with portable route registration API; Arduino backend routes/handlers with fixed-buffer response batching | ESP-IDF, Arduino |
| `bb_log` | Ring-buffered log capture with SSE `/api/logs` endpoint; `bb_log_{e,w,i,d,v}` macros for platform-abstract logging | ESP-IDF, Arduino |
| `bb_nv` | Typed NVS accessors plus generic `bb_nv_*` key/value helpers with caller-supplied namespace | ESP-IDF, Arduino |
| `bb_ota_pull` | HTTP releases-feed poller with cJSON parse and A/B rollback | ESP-IDF |
| `bb_ota_push` | HTTP firmware upload handler | ESP-IDF |
| `bb_ota_validator` | Owns the full OTA rollback state machine: boot-time pending detection, rollback-safety preflight, `bb_ota_mark_valid(reason)` signal API, POST `/api/ota/mark-valid` | ESP-IDF (portable stubs on non-ESP) |
| `bb_board` | Runtime sysinfo (chip model, cores, flash, heap, OTA state) and GET `/api/board` | ESP-IDF |
| `bb_info` | Composite GET `/api/info` merging sysinfo + wifi + consumer-registered extender callbacks | ESP-IDF |
| `bb_mdns` | mDNS service registration with hostname, instance, service-type setters | ESP-IDF |
| `bb_prov` | Provisioning state machine (SoftAP + captive-portal + HTTP `/save` handler) | ESP-IDF |
| `bb_prov_default_form` | Opt-in default WiFi setup form asset (`bb_prov_default_form_asset`) for bare-minimum `bb_prov` bringup | ESP-IDF |
| `bb_wifi` | STA init, async scan, auto-reconnect, diagnostics and GET `/api/wifi` | ESP-IDF |

## Use in an ESP-IDF project

Append the `components/` directory to your project's top-level `CMakeLists.txt`:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS "<path-to>/breadboard/components")
```

Pick individual components in your app's `idf_component_register(... REQUIRES ...)` — you only pay build cost for what you use.

Tagged source archives will be published on the [releases page](https://github.com/dangernoodle-io/breadboard/releases) once the API stabilizes.

## Provisioning UI

The `bb_prov` component manages the provisioning state machine and HTTP `/save` handler. Consumers provide a `prov_ui_routes_fn` callback to `bb_prov_start` to register `GET /` and any static assets (favicon, css, logo). `POST /save` returns `204 No Content`; the caller's form JS is responsible for post-submit UX.

## Portability

Public headers guard `esp_*.h` and `freertos/*.h` behind `#ifdef ESP_PLATFORM` so non-ESP backends (e.g. Arduino) can coexist without breaking consumers. Components are designed portably even when only one backend is fully implemented. Arduino has validated backends for `bb_log`, `bb_nv`, `bb_http`, and the `bb_system_*` helpers driving `/api/version` and `/api/reboot`. `bb_ota_validator` exposes a portable strategy-struct API, with stubs on non-ESP platforms. Other components are currently ESP-IDF-only; progressive un-fencing is planned.

## Development

```bash
make check              # cppcheck static analysis
make coverage           # host unit tests + gcovr → Coveralls-format JSON
make smoke-elecrow-p4-hmi7     # build ESP-IDF example
make smoke-arduino-uno-cc3000  # build Arduino Uno + CC3000 example
make smoke              # build all examples
```

117 host tests under `test/test_host/` cover `bb_log` (macro expansion and ring buffer drain), `bb_nv` (typed and generic), `bb_prov`, `http_utils`, `bb_ota_pull`, `bb_ota_push`, `bb_ota_validator`, and `bb_json` (round-trip build/serialize/parse for all value types, nested trees, arrays, edge cases). The `bb_hw`, `bb_display`, and hardware-coupled components have no host coverage.

See `examples/arduino-uno-cc3000/README.md` for Arduino development setup (Homebrew toolchain on macOS, stock PIO toolchain on Linux).

## License

See [LICENSE](LICENSE) (MIT).

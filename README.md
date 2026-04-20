# breadboard

[![Build](https://github.com/dangernoodle-io/breadboard/actions/workflows/build.yml/badge.svg)](https://github.com/dangernoodle-io/breadboard/actions/workflows/build.yml)
[![Coverage Status](https://coveralls.io/repos/github/dangernoodle-io/breadboard/badge.svg?branch=main)](https://coveralls.io/github/dangernoodle-io/breadboard?branch=main)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Reusable components for embedded systems: wifi provisioning, NVS config, HTTP server, OTA, log streaming, and display/board abstraction. Supports ESP-IDF (production) and Arduino (experimental).

> **Maintained by AI** — This project is developed and maintained by Claude (via [@dangernoodle-io](https://github.com/dangernoodle-io)).
> If you find a bug or have a feature request, please [open an issue](https://github.com/dangernoodle-io/breadboard/issues) with examples so it can be addressed.

**Status:** Pre-release (ESP-IDF APIs stable; Arduino backend beta).

## Components

| Component | Purpose | Platforms |
|-----------|---------|-----------|
| `board` | GPIO / power / boot-mode helpers for the host board | ESP-IDF |
| `display` | MIPI-DSI panel init (EK79007) with LVGL via `esp_lvgl_port`; consumer holds `bb_display_lock` for all LVGL calls. Exposes `bb_display_screen` / `bb_display_lock` / `bb_display_unlock` for direct LVGL access. | ESP-IDF |
| `http_server` | esp_http_server wrapper with portable route registration API; Arduino backend routes/handlers with fixed-buffer response batching | ESP-IDF, Arduino |
| `bb_prov` | Provisioning state machine (SoftAP + captive-portal + HTTP `/save` handler) | ESP-IDF |
| `log_stream` | Ring-buffered log capture for remote retrieval; `bb_log_{e,w,i,d,v}` macros for platform-abstract logging | ESP-IDF, Arduino |
| `nv_config` | Typed NVS accessors (wifi SSID/pass, display enable, boot count, OTA flags) plus generic `bb_nv_*` key/value helpers with caller-supplied namespace | ESP-IDF, Arduino |
| `ota_pull` | HTTP releases-feed poller with cJSON parse and A/B rollback | ESP-IDF |
| `wifi_prov` | SoftAP + captive-portal wifi provisioning flow | ESP-IDF |

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

Public headers guard `esp_*.h` and `freertos/*.h` behind `#ifdef ESP_PLATFORM` so non-ESP backends (e.g. Arduino) can coexist without breaking consumers. The Arduino backend (`platform/arduino/`) is in beta; `log_stream`, `nv_config`, and `http_server` are validated on hardware, while `wifi_prov` is deferred pending API stabilization.

## Development

```bash
make check              # cppcheck static analysis
make coverage           # host unit tests + gcovr → Coveralls-format JSON
make smoke-elecrow-p4-hmi7     # build ESP-IDF example
make smoke-arduino-uno-cc3000  # build Arduino Uno + CC3000 example
make smoke              # build all examples
```

72 host tests under `test/test_host/` cover `log_stream` (with macro expansion tests), `nv_config` (typed and generic), `http_utils`, and `ota_pull`. The `board`, `display`, and `wifi_prov` components are hardware- or BT-coupled and have no host coverage.

See `examples/arduino-uno-cc3000/README.md` for Arduino development setup (Homebrew toolchain on macOS, stock PIO toolchain on Linux).

## License

See [LICENSE](LICENSE) (MIT).

# breadboard

[![Build](https://github.com/dangernoodle-io/breadboard/actions/workflows/build.yml/badge.svg)](https://github.com/dangernoodle-io/breadboard/actions/workflows/build.yml)
[![Coverage Status](https://coveralls.io/repos/github/dangernoodle-io/breadboard/badge.svg?branch=main)](https://coveralls.io/github/dangernoodle-io/breadboard?branch=main)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Reusable ESP-IDF components for wifi provisioning, NVS config, HTTP server, OTA, log streaming, and display/board abstraction.

> **Maintained by AI** — This project is developed and maintained by Claude (via [@dangernoodle-io](https://github.com/dangernoodle-io)).
> If you find a bug or have a feature request, please [open an issue](https://github.com/dangernoodle-io/breadboard/issues) with examples so it can be addressed.

**Status:** Pre-release.

## Components

| Component | Purpose |
|-----------|---------|
| `board` | GPIO / power / boot-mode helpers for the host board |
| `display` | MIPI-DSI panel init (EK79007) with LVGL via `esp_lvgl_port`; consumer holds `bb_display_lock` for all LVGL calls. Exposes `bb_display_screen` / `bb_display_lock` / `bb_display_unlock` for direct LVGL access. |
| `http_server` | esp_http_server wrapper with provisioning state machine (see note below) |
| `log_stream` | Ring-buffered log capture for remote retrieval; `bb_log_{e,w,i,d,v}` macros for platform-abstract logging |
| `nv_config` | Typed NVS accessors (wifi SSID/pass, display enable, boot count, OTA flags) plus generic `bb_nv_*` key/value helpers with caller-supplied namespace |
| `ota_pull` | HTTP releases-feed poller with cJSON parse and A/B rollback |
| `wifi_prov` | SoftAP + captive-portal wifi provisioning flow |

## Use in an ESP-IDF project

Append the `components/` directory to your project's top-level `CMakeLists.txt`:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS "<path-to>/breadboard/components")
```

Pick individual components in your app's `idf_component_register(... REQUIRES ...)` — you only pay build cost for what you use.

Tagged source archives will be published on the [releases page](https://github.com/dangernoodle-io/breadboard/releases) once the API stabilizes.

## Provisioning UI

`http_server` ships the state machine, not the UI. Consumers register `GET /` (and any static assets: favicon, css, logo) via the `prov_ui_routes_fn` callback to `bb_http_server_start_prov`. `POST /save` returns `204 No Content`; the caller's form JS is responsible for post-submit UX.

## Portability

Public headers guard `esp_*.h` and `freertos/*.h` behind `#ifdef ESP_PLATFORM` so a future non-ESP backend (e.g. Arduino) can be added without breaking consumers.

## Development

```bash
make check     # cppcheck static analysis
make coverage  # host unit tests + gcovr → Coveralls-format JSON
make smoke     # pio run -d examples/minimal
```

72 host tests under `test/test_host/` cover `log_stream` (with macro expansion tests), `nv_config` (typed and generic), `http_utils`, and `ota_pull`. The `board`, `display`, and `wifi_prov` components are hardware- or BT-coupled and have no host coverage.

## License

See [LICENSE](LICENSE) (MIT).

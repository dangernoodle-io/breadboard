# ESP-IDF Backend

ESP-IDF-specific implementations for breadboard components. Public API and platform-agnostic code live in `components/<name>/include/` and `components/<name>/src/`; this tree holds the ESP-IDF backend that satisfies those APIs.

## Layout

Each populated component has a directory here with its `.c` sources:

- `log_stream/` — ring buffer + streaming task
- `nv_config/` — NVS-backed config store
- `http_server/` — esp_http_server integration (body-parsing helpers live in `components/http_server/src/http_utils.c`)
- `ota_pull/` — esp_https_ota integration
- `wifi_prov/` — Wi-Fi STA/AP + provisioning state machine
- `display/` — LVGL + MIPI-DSI panel init
- `board/` — pin maps and board-specific init

## Adding a new backend

`platform/<backend>/<component>/` would hold the equivalent sources. Each component's `CMakeLists.txt` points `SRCS` at the active backend.

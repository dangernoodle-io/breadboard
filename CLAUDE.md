# ESP-IDF Bootstrap Component Library

Standalone ESP-IDF bootstrap component library. Reusable wifi provisioning, NVS, HTTP server, OTA, log streaming, display, and board abstraction components for any ESP-IDF firmware project.

## Public symbol prefix

All public C symbols use prefix `bb_`.

## Portability discipline

Public headers must not include `esp_*.h` or `freertos/*.h` outside `#ifdef ESP_PLATFORM`. This is enforced so non-ESP-IDF platform backends (e.g. Arduino) can coexist without breaking consumers.

For the Arduino backend specifically:
- Log format strings (`bb_log_*` calls) should be wrapped in `F()` to force them into PROGMEM to preserve SRAM on tiny platforms (2 KB on Uno). For now, breadboard's own logging is sparse enough that this is deferred — only enforce it when a real project hits SRAM pressure.
- Backend implementations are in `platform/arduino/<component>/` as `.cpp` files (not `.c`), since the Arduino framework and ecosystem (EEPROM, Serial, libraries) are C++ native.

## Logging

Use `bb_log_{e,w,i,d,v}(tag, fmt, ...)` macros for all breadboard component code. On ESP-IDF these expand to `ESP_LOG{E,W,I,D,V}`; on host they map to `fprintf` (debug/verbose compile out to keep test output clean). Consumers (snugfeather, TaipanMiner) may continue using `ESP_LOG*` directly — the abstraction is for breadboard internal portability only.

## Layout

- Components under `components/<name>/`.
- Platform-specific impl under `platform/espidf/` (currently the only backend).

## Provisioning UI

The `http_server` component does NOT ship a provisioning UI. Consumers must register `GET /` (and any static assets) via the `prov_ui_routes_fn` callback to `bb_http_server_start_prov`. `POST /save` returns `204 No Content`; the caller's form JS is responsible for post-submit UX.

## Display

LVGL is initialized inside `bb_display_init` via `esp_lvgl_port`. The consumer's `sdkconfig` governs LVGL font availability (`CONFIG_LV_FONT_MONTSERRAT_*`) and color depth (must be 16-bit / RGB565); breadboard does not ship pre-defined dashboard layouts. Every call into LVGL from application code (including `lv_timer` callbacks not running on the LVGL task) must be wrapped in `bb_display_lock` / `bb_display_unlock`.

## Workspace conventions

Workspace-level conventions (git, testing, docs) live in `/Users/jae/Projects/dangernoodle/CLAUDE.md`.

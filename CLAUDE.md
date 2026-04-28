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
- **Log via `bb_log_*`**, never `ESP_LOG*`. Portable impls live under `platform/<backend>/bb_log/`.
- **No `#ifdef ESP_PLATFORM` in public headers.** If a function needs an ESP-IDF type in its signature, wrap it behind an opaque `bb_*` handle (see `bb_http_request_t` wrapping `httpd_req_t *`).
- **No ESP-IDF headers** (`esp_err.h`, `esp_http_server.h`, `nvs_flash.h`, …) included transitively from public headers. Private `.c` files may include them freely.

When extending an existing component: match the existing style even if older code predates these rules; a PR that flips 10 functions without migrating the rest is churn.

## Logging

Use `bb_log_{e,w,i,d,v}(tag, fmt, ...)` macros for all breadboard component code. On ESP-IDF these expand to `ESP_LOG{E,W,I,D,V}`; on host they map to `fprintf` (debug/verbose compile out to keep test output clean).

## Layout

- Components under `components/<name>/` with public headers.
- Platform-specific implementations under `platform/espidf/` and `platform/arduino/`.
- Component names are prefixed `bb_`.

## Embedding assets

Use the CMake helper `bb_embed_assets()` in `cmake/bb_embed.cmake` to embed binary assets (HTML, fonts, images, etc.) into firmware. The helper wraps the raw `scripts/embed_html.py` CLI tool, which converts a file to a gzipped C byte-array source file: `python3 scripts/embed_html.py <input> <output.c> <symbol>`. Components include the helper with `include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_embed.cmake")` and call it before `idf_component_register` to populate `SRCS`. The helper avoids duplicating `add_custom_command` boilerplate across breadboard components and downstream consumers (TaipanMiner, snugfeather).

## Registry (handler-lifecycle)

The `bb_registry` component holds a list of `(name, init_fn)` entries that opt-in components self-register at link time via the `BB_REGISTRY_REGISTER(name, init_fn)` macro. The app calls `bb_registry_init(server)` once after `bb_http_server_start`; the registry walks every entry and invokes its init.

Pattern in a component source file:
```c
#include "bb_registry.h"
static bb_err_t bb_<comp>_init(bb_http_handle_t server) {
    // existing route registration body
    return BB_OK;
}
BB_REGISTRY_REGISTER(bb_<comp>, bb_<comp>_init);
```

The macro emits a `__attribute__((constructor))` that runs before `app_main`. The component still has to appear in the app's CMake `REQUIRES` — without it, the linker drops the archive and the constructor never fires. What goes away is the public `bb_<comp>_register_handler(server)` function the app used to call.

The component's `CMakeLists.txt` must also call the `bb_registry_force_register()` helper (in `cmake/bb_registry.cmake`) once per registry entry. Why: PlatformIO's espidf builder strips ESP-IDF's `WHOLE_ARCHIVE` flag, so without an external symbol reference (`-u <symbol>`), the linker's `--gc-sections` discards the entire constructor-only translation unit and the registry sees zero entries at runtime. The helper passes `-u bb_registry_register__<name>` to the linker.

```cmake
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
bb_registry_force_register(${COMPONENT_LIB} bb_<name>)
```

The `<name>` argument must match the first arg of the component's `BB_REGISTRY_REGISTER(<name>, ...)` macro call. Components with multiple registry entries call the helper once per entry.

Auto-registration is opt-out via Kconfig. Each registry-using component defines a `CONFIG_BB_<NAME>_AUTOREGISTER` flag (default-on) that gates both the `BB_REGISTRY_REGISTER` invocation in source (`#if CONFIG_BB_..._AUTOREGISTER`) and the `bb_registry_force_register` call in CMake (`if(CONFIG_BB_..._AUTOREGISTER)`). The CMake gate is required: without it, the linker would inject `-u bb_registry_register__<name>` for a symbol the source side just disabled. Disabling the flag preserves the component's public C API but removes the route registration from `bb_registry_init(server)`.

Today this pattern owns: `bb_ota_pull` (`CONFIG_BB_OTA_PULL_AUTOREGISTER`), `bb_ota_push` (`CONFIG_BB_OTA_PUSH_AUTOREGISTER`), `bb_info` (`CONFIG_BB_INFO_AUTOREGISTER`), `bb_log` (routes module gated by `CONFIG_BB_LOG_ROUTES`), `bb_board` (`CONFIG_BB_BOARD_AUTOREGISTER`), `bb_manifest` (`CONFIG_BB_MANIFEST_AUTOREGISTER`), `bb_ota_validator` (`CONFIG_BB_OTA_VALIDATOR_AUTOREGISTER`), `bb_wifi` (routes module gated by `CONFIG_BB_WIFI_ROUTES_AUTOREGISTER`), `bb_system` (routes module gated by `CONFIG_BB_SYSTEM_ROUTES_AUTOREGISTER`), `bb_openapi` (`CONFIG_BB_OPENAPI_AUTOREGISTER`), and `bb_mdns` (`CONFIG_BB_MDNS_AUTOREGISTER`). When a component's routes are disabled, the routes source files and HTTP/JSON/registry dependencies are dropped from PRIV_REQUIRES — see bb_log's pattern for reference. Use the registry pattern for any component that does pure, no-state route registration. Components that need caller-supplied state (`bb_prov` assets, `bb_wifi` creds) stay caller-driven; `bb_mdns` setters (hostname, service type, instance name) are still caller-driven but init is now registry-controlled.

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

## Releases

Tagging is manual: `git tag -a vX.Y.Z -m 'chore: vX.Y.Z tag' && git push origin vX.Y.Z`. The `release.yml` workflow waits for CI then publishes a GitHub release with auto-generated notes categorized by PR label (`.github/release.yml`). PR labels are auto-applied from conventional-commit prefixes; `new-component` PRs need that label set manually.

## Workspace conventions

Workspace-level conventions (git, testing, docs) live in `/Users/jae/Projects/dangernoodle/CLAUDE.md`.

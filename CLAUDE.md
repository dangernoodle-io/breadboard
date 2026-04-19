# ESP-IDF Bootstrap Component Library

Standalone ESP-IDF bootstrap component library. Reusable wifi provisioning, NVS, HTTP server, OTA, log streaming, display, and board abstraction components for any ESP-IDF firmware project.

## Working symbol prefix

All public C symbols use prefix `bsp_` as a working placeholder. This will be globally renamed when the library is named for publish.

## Portability discipline

Public headers must not include `esp_*.h` or `freertos/*.h` outside `#ifdef ESP_PLATFORM`. This is enforced so a future non-ESP-IDF platform backend (e.g. Arduino) can be added without breaking consumers.

## Layout

- Components under `components/<name>/`.
- Platform-specific impl under `platform/espidf/` (currently the only backend).

## Provisioning UI

The `http_server` component does NOT ship a provisioning UI. Consumers must register `GET /` (and any static assets) via the `prov_ui_routes_fn` callback to `bsp_http_server_start_prov`. `POST /save` returns `204 No Content`; the caller's form JS is responsible for post-submit UX.

## Workspace conventions

Workspace-level conventions (git, testing, docs) live in `/Users/jae/Projects/dangernoodle/CLAUDE.md`.

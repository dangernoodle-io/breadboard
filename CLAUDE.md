# ESP-IDF Bootstrap Component Library

Standalone ESP-IDF bootstrap component library. Reusable wifi provisioning, NVS, HTTP server, OTA, log streaming, display, and board abstraction components for any ESP-IDF firmware project.

## Working symbol prefix

All public C symbols use prefix `bsp_` as a working placeholder. This will be globally renamed when the library is named for publish.

## Portability discipline

Public headers must not include `esp_*.h` or `freertos/*.h` outside `#ifdef ESP_PLATFORM`. This is enforced so a future non-ESP-IDF platform backend (e.g. Arduino) can be added without breaking consumers.

## Layout

- Components under `components/<name>/`.
- Platform-specific impl under `platform/espidf/` (currently the only backend).

## Workspace conventions

Workspace-level conventions (git, testing, docs) live in `/Users/jae/Projects/dangernoodle/CLAUDE.md`.

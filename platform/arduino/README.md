# Arduino Backend

Arduino-framework-specific implementations for breadboard components. Public API and platform-agnostic code live in `components/<name>/include/` and `components/<name>/src/`; this tree holds the Arduino backend that satisfies those APIs.

## Layout

Each populated component has a directory here with its `.cpp` sources:

- `log_stream/` — Serial-backed logging
- `nv_config/` — EEPROM-backed config store

## Adding a new backend

`platform/<backend>/<component>/` would hold the equivalent sources. Each component's `CMakeLists.txt` points `SRCS` at the active backend.

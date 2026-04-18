# ESP-IDF Backend

This directory holds ESP-IDF-specific implementations for components whose public API is platform-agnostic.

Currently ESP-IDF is the only supported backend. A future Arduino backend would live at `platform/arduino/` alongside.

Most current components keep their ESP-IDF impl directly in `components/<name>/src/` for simplicity. This directory exists to reserve the seam.

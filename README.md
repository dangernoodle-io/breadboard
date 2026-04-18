# ESP-IDF Bootstrap Component Library

Reusable wifi provisioning, NVS, HTTP server, OTA, log streaming, display, and board abstraction components for any ESP-IDF firmware project. Designed for rapid bootstrapping of embedded systems on Espressif chips with common networking, storage, and utility infrastructure.

**Status:** Pre-release, pre-naming. API and symbol prefix are subject to rename before first publish.

## Consuming in an ESP-IDF project

Add the library components to your project by appending to the top-level `CMakeLists.txt`:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS "<path-to>/lib-tbd/components")
```

Then reference individual components in your `idf_component_register()` calls as you would any ESP-IDF component.

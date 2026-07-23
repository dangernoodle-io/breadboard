# storage

Key/value storage primitives — `bb_storage` (the portable facade + backend
registry) plus its backends (`bb_storage_nvs`, `bb_storage_ram`,
`bb_storage_rtc`), each registering with `bb_storage` via the
backend-dispatch convention rather than being a standalone consumer-facing
API.

<!-- BEGIN bbtool:group-index -->
| Component | Purpose |
|-----------|---------|
| [bb_storage](./bb_storage/) | Portable storage facade + backend registry: one `bb_storage_get/set/erase/exists` API dispatching by `bb_storage_addr_t.backend` to whichever backend has registered itself. |
| [bb_storage_nvs](./bb_storage_nvs/) | ESP-IDF NVS backend for `bb_storage`. |
| [bb_storage_ram](./bb_storage_ram/) | In-memory `bb_storage` backend — the reference implementation, a fixed-capacity key/value table with no heap allocation. |
| [bb_storage_rtc](./bb_storage_rtc/) | Warm-reboot RTC-mirror `bb_storage` backend for WiFi credentials. |
<!-- END bbtool:group-index -->

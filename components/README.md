# Components

This directory holds breadboard's reusable components. Each `components/<name>/`
directory is the **portable** surface for one component: public headers, Kconfig,
and CMakeLists. Platform-specific implementations live under
`platform/{host,espidf,arduino}/<name>/` (sibling to `components/`, not nested
inside it) — the portable component declares the API, the platform tree provides
the backend(s).

See the top-level [README](../README.md) and [CLAUDE.md](../CLAUDE.md) for
project-wide conventions, build instructions, and architecture notes.

## Index

| Component | Purpose |
|-----------|---------|
| [bb_attrs](./bb_attrs/) | Intrusive header carrying filter/collection metadata (`priority`, `kind`, `tag_mask`, `delivery_class`) that any element embeds as a member. Reach for it when a set of heterogeneous elements needs to be projected/selected by `bb_filter` without a shared base type or heap-allocated wrapper. |
| [bb_board](./bb_board/) | — |
| [bb_bqueue](./bb_bqueue/) | Blocking mailbox/MPSC queue — capacity==1 selects mailbox mode (overwrite/reset), capacity>1 selects bounded-MPSC mode (send/dropped); peek/receive/count/capacity work in both. Zero heap: a Kconfig-sized static instance pool. |
| [bb_button](./bb_button/) | — |
| [bb_button_events](./bb_button_events/) | — |
| [bb_button_gpio](./bb_button_gpio/) | — |
| [bb_cache](./bb_cache/) | — |
| [bb_cache_reactive](./bb_cache_reactive/) | — |
| [bb_cache_routes](./bb_cache_routes/) | — |
| [bb_cache_serialize](./bb_cache_serialize/) | Compositional serialized-render cache (render memo), keyed (format_id, key, state_version). Format-agnostic: dispatches rendering through the format registry (bb_serialize_format) and deliberately declares NO dependency on any format backend. Rendering via bb_cache_serialize_get() therefore requires the composition to have registered a bb_serialize_* backend for the requested format -- an inherent property of registry dispatch, not a privileged default. A consumer that does not render through this path is unaffected. |
| [bb_collection](./bb_collection/) | A humble, fixed-capacity, thread-safe ordered collection of caller-owned opaque items. |
| [bb_config](./bb_config/) | Typed configuration layer over `bb_storage` — gives its blob-only vtable scalar-typed meaning (bool/u8/u16/u32/i32/str/blob) via a caller-owned field descriptor table. |
| [bb_core](./bb_core/) | Foundational, near-zero-dep primitives every bb_* component builds on: the portable error type, the canonical clock, run-exactly-once, a contention-instrumented lock, byte-order helpers, memory accounting, and the reboot-reason codec. |
| [bb_data](./bb_data/) | bb_data core binding table (B1-832) -- OWNS the `key -> (desc, gather)` binding table for the future bidirectional data path (the B1-828 epic replacing bb_pub + bb_sub + all bb_sink_*). DIRECT: bb_data delegates ONLY the wire-format step to the existing bb_serialize format-dispatch registry (bb_serialize_format.h) -- it does NOT wrap bb_cache or bb_cache_serialize, and has no dependency on either. |
| [bb_data_http](./bb_data_http/) | bb_data_http -- the converged HTTP SSE/WS push transport (B1-1033, design KB 1443/1444). Dep-light pure core (bb_queue + bb_core only): it NEVER links bb_data or bb_ws_server directly. Instead it calls three INJECTED function-pointer seams -- render, generation-read, and send -- that the composition root wires to real bb_data / bb_ws_server / httpd calls (ESP-IDF) or to test doubles (host). This keeps bb_data_http free to serve any egress transport without a hard dependency on the data or websocket layers. |
| [bb_diag](./bb_diag/) | — |
| [bb_display](./bb_display/) | — |
| [bb_display_ek79007](./bb_display_ek79007/) | — |
| [bb_display_ili9341](./bb_display_ili9341/) | — |
| [bb_display_spi_common](./bb_display_spi_common/) | — |
| [bb_display_ssd1306](./bb_display_ssd1306/) | — |
| [bb_display_st77xx](./bb_display_st77xx/) | — |
| [bb_fan](./bb_fan/) | — |
| [bb_fan_emc2101](./bb_fan_emc2101/) | — |
| [bb_filter](./bb_filter/) | Pure projection over elements carrying `bb_attrs`: given an array of `{attrs, item}` pairs and a selector, returns the matching, priority-sorted subset. Reach for it whenever a consumer needs to pick "the top N by priority/kind/tag" out of a candidate set without building a registry. |
| [bb_fmt](./bb_fmt/) | — |
| [bb_fsm](./bb_fsm/) | Table-driven finite state machine primitive: consumer-owned rows (state, event, guard, action, next), entry/exit hooks, and a fixed-size timer-arm seam for the shell to reconstruct real OS timers from. Pure per-instance library -- no autoinit, no global state, no lock, embedded by value in the consumer's own struct. |
| [bb_health](./bb_health/) | — |
| [bb_http](./bb_http/) | — |
| [bb_http_client](./bb_http_client/) | — |
| [bb_http_server](./bb_http_server/) | — |
| [bb_i2c](./bb_i2c/) | — |
| [bb_json](./bb_json/) | — |
| [bb_led](./bb_led/) | — |
| [bb_led_anim](./bb_led_anim/) | — |
| [bb_led_apa102](./bb_led_apa102/) | — |
| [bb_led_gpio](./bb_led_gpio/) | — |
| [bb_led_pwm](./bb_led_pwm/) | — |
| [bb_led_rgb_pwm](./bb_led_rgb_pwm/) | — |
| [bb_lifecycle](./bb_lifecycle/) | Service run-state authority: register named services, track a computed STOPPED/PAUSED/RUNNING state per service, and let independent subsystems assert/clear open-vocabulary pause reasons without stepping on each other. PUSH (observer), PULL (generic emit sink), and POLL (lock-free version counter) delivery, all sourced from one lock-guarded commit. |
| [bb_log](./bb_log/) | — |
| [bb_log_event](./bb_log_event/) | "log" `bb_event` stream topic sink, carved out of `bb_log` (KB #708/#704). |
| [bb_log_http](./bb_log_http/) | GET/POST `/api/log/level` routes sink, carved out of `bb_log` (KB #708/#704). |
| [bb_manifest](./bb_manifest/) | — |
| [bb_mdns](./bb_mdns/) | — |
| [bb_mdns_cache](./bb_mdns_cache/) | — |
| [bb_mem_arena](./bb_mem_arena/) | — |
| [bb_mem_arena_tls](./bb_mem_arena_tls/) | — |
| [bb_meminfo](./bb_meminfo/) | Canonical system-heap reader SSOT (KB #698/#699/#693) — the one component that |
| [bb_mqtt_client](./bb_mqtt_client/) | — |
| [bb_mqtt_info](./bb_mqtt_info/) | — |
| [bb_ntp](./bb_ntp/) | — |
| [bb_num](./bb_num/) | — |
| [bb_openapi](./bb_openapi/) | — |
| [bb_ota_boot](./bb_ota_boot/) | — |
| [bb_ota_check](./bb_ota_check/) | — |
| [bb_ota_hooks](./bb_ota_hooks/) | — |
| [bb_ota_pull](./bb_ota_pull/) | — |
| [bb_ota_push](./bb_ota_push/) | — |
| [bb_ota_validator](./bb_ota_validator/) | — |
| [bb_partition](./bb_partition/) | — |
| [bb_pool](./bb_pool/) | — |
| [bb_power](./bb_power/) | — |
| [bb_power_health](./bb_power_health/) | — |
| [bb_power_tps546](./bb_power_tps546/) | — |
| [bb_prov_default_form](./bb_prov_default_form/) | — |
| [bb_queue](./bb_queue/) | — |
| [bb_registry](./bb_registry/) | — |
| [bb_release_manifest](./bb_release_manifest/) | — |
| [bb_response](./bb_response/) | — |
| [bb_ring_diag](./bb_ring_diag/) | — |
| [bb_scalar](./bb_scalar/) | — |
| [bb_sensors](./bb_sensors/) | — |
| [bb_serialize](./bb_serialize/) | Format-neutral snapshot serialization: a descriptor SSOT + a pure walker + the bb_serialize_emit_t emit-vtable seam. |
| [bb_serialize_console](./bb_serialize_console/) | Heap-over-serial emit backend -- a bb_serialize_emit_t implementation that renders a snapshot as a single human-readable "key=val key=val" line (no braces, no quoting), plus a one-shot heap-snapshot report helper (bb_serialize_console_heap_report()) built on top of bb_meminfo. |
| [bb_serialize_json](./bb_serialize_json/) | Hand-rolled, no-heap, bounded-buffer JSON bb_serialize_emit_t backend -- the default wire-format implementation for bb_serialize. |
| [bb_serialize_logfmt](./bb_serialize_logfmt/) | Hand-rolled, no-heap, bounded-buffer logfmt bb_serialize_emit_t backend -- a second wire-format implementation for bb_serialize, mirroring bb_serialize_json's structure and contract shape. |
| [bb_settings](./bb_settings/) | bb's default WiFi-credentials store — a wifi-creds field table over `bb_config`, byte-compatible with the credentials `bb_nv_config` already persists. `bb_settings` is bb's opinionated bb-config authority (KB 805/806); `bb_wifi` reads its accessors directly. |
| [bb_storage](./bb_storage/) | Portable storage facade + backend registry: one `bb_storage_get/set/erase/exists` API dispatching by `bb_storage_addr_t.backend` to whichever backend has registered itself. |
| [bb_storage_http](./bb_storage_http/) | Backend-agnostic DELETE /api/diag/storage route over the bb_storage facade, plus POST /api/diag/factory-reset (whole-partition erase + reboot) — works for any registered backend (nvs today, ram/rtc/a future sdcard) with no new route component per backend. |
| [bb_storage_nvs](./bb_storage_nvs/) | ESP-IDF NVS backend for `bb_storage`. |
| [bb_storage_ram](./bb_storage_ram/) | In-memory `bb_storage` backend — the reference implementation, a fixed-capacity key/value table with no heap allocation. |
| [bb_storage_rtc](./bb_storage_rtc/) | Warm-reboot RTC-mirror `bb_storage` backend for WiFi credentials. |
| [bb_str](./bb_str/) | — |
| [bb_system](./bb_system/) | — |
| [bb_task](./bb_task/) | — |
| [bb_task_registry](./bb_task_registry/) | — |
| [bb_tcp_client](./bb_tcp_client/) | Portable connected TCP/TLS stream client — the stream peer to bb_udp_client. Flat per-platform-TU dispatch (see wiki Backend-Dispatch). |
| [bb_temp](./bb_temp/) | — |
| [bb_thermal](./bb_thermal/) | — |
| [bb_timer](./bb_timer/) | — |
| [bb_tls](./bb_tls/) | — |
| [bb_tls_creds](./bb_tls_creds/) | — |
| [bb_udp_client](./bb_udp_client/) | Reusable IPv4 UDP datagram transport (ouroboros KB#702/#710) — the datagram peer to bb_tcp_client. |
| [bb_udp_frame](./bb_udp_frame/) | — |
| [bb_wdt](./bb_wdt/) | — |
| [bb_wifi](./bb_wifi/) | STA WiFi core: connect/reconnect lifecycle, a self-heal reconnect FSM that recovers from disconnects and no-IP stalls without a reboot, and portable diagnostics getters (RSSI, disconnect reason, scan results) that every backend (ESP-IDF, CC3000, WiFiS3/R4, host) maps onto. |
| [bb_wifi_ap](./bb_wifi_ap/) | bb_wifi_ap — SoftAP + captive-DNS primitive (KB 781): pure AP bring-up and a captive DNS responder, zero HTTP. Extracted from bb_prov's former AP code; bb_prov does not call bb_wifi_ap_start()/stop(). It is a standalone primitive — callers (or the future bb_wifi_prov lifecycle FSM) invoke bb_wifi_ap_start()/stop() themselves; nothing wires it into bb_prov automatically. The AP<->STA lifecycle FSM, net-event topics, and recovery model are out of scope here — see bb_wifi_prov for provisioning orchestration. |
| [bb_wifi_http](./bb_wifi_http/) | Opt-in STA route bundle for `bb_wifi` (PR1 of the bb_wifi split, KB 781/809). |
| [bb_wifi_prov](./bb_wifi_prov/) | bb_wifi_prov — Wi-Fi provisioning HTTP routes: parses a POSTed SSID/password form and a captive-portal redirect. Registers POST /save and a captive GET /* wildcard on the shared HTTP server; does not register /api/version, /api/scan, or /api/reboot (those live in bb_wifi_http / bb_system), and does not itself bring up SoftAP or drive a Wi-Fi lifecycle state machine (see bb_wifi_ap for AP bring-up). |
| [bb_ws_server](./bb_ws_server/) | — |

---
_Generated by `scripts/gen_components_readme.py` — see [doc conventions](../wiki/Component-Docs)._

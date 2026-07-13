// ESP-IDF SPIRAM allocator for bb_event_ring (post-consolidation stub).
//
// bb_event_ring now stores all entries via bb_queue internally.  SPIRAM
// preference for that storage is handled by the bb_queue_espidf component
// (platform/espidf/bb_queue_espidf/bb_queue_spiram.c), which registers a single
// EARLY-tier hook that calls bb_queue_set_allocator.
//
// This file is kept as a no-op shim so that existing consumer CMakeLists that
// list bb_event_ring_espidf in EXTRA_COMPONENT_DIRS continue to compile without
// modification.  The component still contributes bb_event_ring_clock.c (the
// esp_timer-backed clock) via its CMakeLists — only the allocator registration
// has been removed.
//
// If a consumer does NOT include bb_queue_espidf in its build, it should add it
// to ensure event-ring storage lands in SPIRAM on boards with PSRAM.

// Intentionally empty — no registration, no allocator override here.

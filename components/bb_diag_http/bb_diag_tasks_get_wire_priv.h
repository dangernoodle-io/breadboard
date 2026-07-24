#pragma once

// bb_diag_tasks_get_wire — private wire descriptor (SSOT) for the GET
// /api/diag/tasks response (B1-1191 diag conversion, B1-1054 stream).
// Migration of tasks_get_handler's hand-streamed bb_http_resp_json_obj_*
// emitter (platform/espidf/bb_diag_http/bb_diag_http_routes.c) to a
// bb_serialize descriptor rendered via bb_http_serialize_stream(). Locked
// design (do NOT reintroduce a fork):
//
// Shape (mirrors s_tasks_get_responses[]'s hand-authored OpenAPI literal,
// same file):
//   {"tasks":[{"name","prio","base_prio","stack_hwm","state"
//              [,"core"][,"runtime"]
//              [,"stack_budget_bytes","wdt_subscribed"]
//              [,"sw_wdt_timeout_ms","sw_wdt_last_feed_age_ms",
//                "sw_wdt_miss_count","sw_wdt_last_miss_age_ms"]},...],
//    "registry":{"count","capacity","dropped"}}
//
// NO descriptor `#if` fork. `core`/`runtime` genuinely vanish as
// TaskStatus_t members when CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID /
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS are off -- that Kconfig gate
// STAYS in tasks_get_handler's ESP-IDF-only gather (inside the existing
// #if CONFIG_FREERTOS_USE_TRACE_FACILITY block), which resolves each
// gated field to a plain (bool present, int64_t value) pair BEFORE calling
// bb_diag_tasks_get_wire_fill_row() below. This file (descriptor + fill)
// carries ZERO `#if CONFIG_*` -- a single unconditional row shape, gated
// entirely by 4 precomputed present-flags baked into the row struct
// itself.
//
// Per-row present conditions (see bb_diag_tasks_get_wire.c for the
// predicate fns, all of which read only the row struct passed to them --
// this works inside a BB_ARR_STREAM row exactly like a top-level `.present`
// predicate, since bb_serialize_walk()'s STREAM branch calls
// `f->present(row_buf)`, the same one-arg contract as every other shape):
//   name/prio/base_prio/stack_hwm/state -- always (unconditional)
//   core     -- core_present (CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID)
//   runtime  -- runtime_present (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)
//   stack_budget_bytes/wdt_subscribed -- registry_present
//     (bb_task_registry_lookup_budget() found this task by name)
//   sw_wdt_timeout_ms/sw_wdt_last_feed_age_ms/sw_wdt_miss_count/
//   sw_wdt_last_miss_age_ms -- sw_wdt_present
//     (bb_task_registry_lookup_sw_wdt() found this task with a nonzero
//     sw_wdt_timeout_ms)
//
// "registry" (count/capacity/dropped) is unconditional/required -- sourced
// from bb_task_base_{count,capacity,dropped}(), independent of the live
// FreeRTOS task list above.
//
// BB_ARR_STREAM, single-pass: uxTaskGetNumberOfTasks() gives an exact `n`
// up front (no two-phase count-then-fill dispatcher round-trip, unlike
// bb_diag_storage_nvs_iter()'s live-NVS-inventory precedent -- see
// bb_diag_tasks_get_wire_fill_snap()'s doc below).
//
// bb_diag_tasks_get_wire_fill_row()/bb_diag_tasks_get_wire_fill_snap() are
// pure populate helpers -- no FreeRTOS/ESP-IDF symbols -- that take the
// already-gathered raw values (the FreeRTOS-only uxTaskGetSystemState()
// walk + bb_task_registry_lookup_* calls stay in tasks_get_handler) and
// produce a host-testable snapshot. Portable: compiles on host + ESP-IDF.
//
// Included by:
//   - platform/espidf/bb_diag_http/bb_diag_http_routes.c (the live handler)
//   - test/test_host/test_bb_diag_tasks_get_wire.c (expected-JSON fixtures)
//   - test/test_host/test_bb_diag_tasks_get_wire_meta_golden.c (meta golden)

#include "bb_serialize.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fixed capacity for the "name" field -- a plain #define, deliberately NOT
// bridged to configMAX_TASK_NAME_LEN (unlike BB_DIAG_SOCKETS_ROW_CAP's
// CONFIG_LWIP_MAX_SOCKETS bridge): the pre-migration handler's default
// deployment already runs at the stock 16-byte FreeRTOS task-name cap, and
// this is a locked design decision (see the B1-1191 architect brief), not
// an oversight.
#define BB_DIAG_TASKS_GET_NAME_MAX 16

// ---------------------------------------------------------------------------
// Row source/wire struct -- one entry in the "tasks" BB_ARR_STREAM array.
// `state` is a BORROWED bb_serialize_str_n_t pointing at a static task-state
// name literal (see bb_diag_tasks_get_wire.c) -- same convention as
// bb_diag_sockets_pcb_wire_t's `state` field.
// ---------------------------------------------------------------------------

typedef struct {
    char                  name[BB_DIAG_TASKS_GET_NAME_MAX];
    int64_t               prio;
    int64_t               base_prio;
    int64_t               stack_hwm;
    bb_serialize_str_n_t  state;

    int64_t               core;
    int64_t               runtime;

    uint64_t              stack_budget_bytes;
    bool                  wdt_subscribed;

    uint64_t              sw_wdt_timeout_ms;
    uint64_t              sw_wdt_last_feed_age_ms;
    uint64_t              sw_wdt_miss_count;
    uint64_t              sw_wdt_last_miss_age_ms;

    // Precomputed present-flags -- the walker's `.present` predicate can
    // only inspect the row it's handed, it cannot itself call
    // bb_task_registry_lookup_budget()/bb_task_registry_lookup_sw_wdt() or
    // branch on a Kconfig #if -- so bb_diag_tasks_get_wire_fill_row()
    // resolves these ahead of the walk.
    bool core_present;
    bool runtime_present;
    bool registry_present;
    bool sw_wdt_present;
} bb_diag_tasks_get_wire_row_t;

// Row descriptor (13 fields) -- shared by the production handler and the
// host tests.
extern const bb_serialize_field_t bb_diag_tasks_get_wire_row_fields[13];
// SSOT field count, computed from the array above -- mirrors
// bb_ota_validator_partition_wire_n_fields's pattern. Callers pass this,
// never a hand-typed literal, so the count can never desync from the array.
extern const uint16_t             bb_diag_tasks_get_wire_row_n_fields;

// ---------------------------------------------------------------------------
// "registry" nested object -- unconditional/required, 3 u64 fields.
// ---------------------------------------------------------------------------

typedef struct {
    uint64_t count;
    uint64_t capacity;
    uint64_t dropped;
} bb_diag_tasks_get_wire_registry_t;

// ---------------------------------------------------------------------------
// Top-level snapshot
// ---------------------------------------------------------------------------

typedef struct {
    bb_serialize_arr_stream_t          tasks;
    bb_serialize_arr_buf_iter_t        tasks_iter_state;  // MUST outlive the walk -- see bb_serialize.h
    bb_diag_tasks_get_wire_registry_t  registry;
} bb_diag_tasks_get_wire_t;

// Top-level object descriptor: renders the shape documented above via
// bb_http_serialize_stream()/bb_serialize_json_render() -- byte-identical to
// the pre-migration hand cJSON emitter.
extern const bb_serialize_desc_t bb_diag_tasks_get_wire_desc;

// bb_serialize_desc_meta_t companion (B1-1059 PR-3a meta-derivation feeder)
// -- co-located JSON Schema docs/validation table for
// bb_diag_tasks_get_wire_desc above, same #if-gated pattern as
// bb_diag_panic_get_wire_priv.h's exemplar. BB_SERIALIZE_META_HOST is a
// host-only define (set by the PlatformIO native env; see platformio.ini) --
// NEVER set by the ESP-IDF/device build, so this declaration (and its
// definition in bb_diag_tasks_get_wire.c) compiles to nothing on-device.
#include "bb_serialize_meta.h"
#if defined(BB_SERIALIZE_META_SHIP)

extern const bb_serialize_desc_meta_t bb_diag_tasks_get_wire_meta;
#endif /* BB_SERIALIZE_META_SHIP */

// Pure row-fill helper: zero-inits `row`, widens/copies the already-gathered
// scalar values, wires `state` to a BORROWED static state-name literal, and
// records the 4 precomputed present-flags. No FreeRTOS/ESP-IDF symbols --
// host-testable without a live task list. `state_name` MUST point at
// static-storage-duration text (a string literal or a static table entry --
// never a stack buffer), mirroring bb_diag_sockets_get_wire_copy_rows()'s
// `state` convention.
void bb_diag_tasks_get_wire_fill_row(bb_diag_tasks_get_wire_row_t *row,
                                      const char *name, int64_t prio, int64_t base_prio,
                                      int64_t stack_hwm, const char *state_name,
                                      bool core_present, int64_t core,
                                      bool runtime_present, int64_t runtime,
                                      bool registry_present, uint64_t stack_budget_bytes,
                                      bool wdt_subscribed,
                                      bool sw_wdt_present, uint64_t sw_wdt_timeout_ms,
                                      uint64_t sw_wdt_last_feed_age_ms,
                                      uint64_t sw_wdt_miss_count,
                                      uint64_t sw_wdt_last_miss_age_ms);

// Pure snap-fill helper: zero-inits `dst`, sets the 3 "registry" scalars,
// and wires `dst->tasks` to stream over `rows` (a caller-owned array of
// `n_rows` ALREADY-FILLED bb_diag_tasks_get_wire_row_t entries, via
// bb_diag_tasks_get_wire_fill_row() above) using
// bb_serialize_arr_stream_from_buf(). `rows` MUST outlive the subsequent
// render/emit call (it is read lazily by the stream iterator, not copied
// here) -- same single-pass contract as tasks_get_handler's own
// TaskStatus_t/row-array lifetime (freed only after the stream render
// completes). No FreeRTOS/ESP-IDF symbols -- host-testable without a live
// task list.
void bb_diag_tasks_get_wire_fill_snap(bb_diag_tasks_get_wire_t *dst,
                                       const bb_diag_tasks_get_wire_row_t *rows, size_t n_rows,
                                       uint64_t registry_count, uint64_t registry_capacity,
                                       uint64_t registry_dropped);

#ifdef __cplusplus
}
#endif

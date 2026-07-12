// bb_pub_info_wire — v2 wire descriptor (SSOT) for the InfoTelemetry
// envelope {"ts_ms":N,"data":{...}}. See bb_pub_info_wire_priv.h for the
// byte-fidelity contract. Compiles on both host and ESP-IDF; no
// platform-specific code.

#include "../bb_pub_info_wire_priv.h"

#include <stddef.h>

// present() predicate for psram_free/psram_total. The walker hands a
// BB_TYPE_OBJ field's `present` predicate the NESTED struct's base address
// (the "data" object itself), NOT the envelope root -- confirmed against
// bb_serialize_walk.c's walk_fields(): the BB_TYPE_OBJ case recurses via
// walk_fields(f->children, f->n_children, p, ...) where `p` is the OBJ
// field's own offset-derived pointer. So this predicate casts snap directly
// to bb_info_telem_wire_t*, not the envelope type.
static bool tw_has_psram(const void *snap)
{
    return ((const bb_info_telem_wire_t *)snap)->has_psram;
}

static const bb_serialize_field_t s_info_telem_data_fields[] = {
    { .key = "heap_internal_free", .type = BB_TYPE_U64,
      .offset = offsetof(bb_info_telem_wire_t, heap_internal_free) },
    { .key = "heap_internal_total", .type = BB_TYPE_U64,
      .offset = offsetof(bb_info_telem_wire_t, heap_internal_total) },
    { .key = "heap_internal_largest_block", .type = BB_TYPE_U64,
      .offset = offsetof(bb_info_telem_wire_t, heap_internal_largest_block) },
    { .key = "heap_internal_min_free", .type = BB_TYPE_U64,
      .offset = offsetof(bb_info_telem_wire_t, heap_internal_min_free) },
    { .key = "psram_free", .type = BB_TYPE_U64,
      .offset = offsetof(bb_info_telem_wire_t, psram_free), .present = tw_has_psram },
    { .key = "psram_total", .type = BB_TYPE_U64,
      .offset = offsetof(bb_info_telem_wire_t, psram_total), .present = tw_has_psram },
    { .key = "wdt_resets", .type = BB_TYPE_U64,
      .offset = offsetof(bb_info_telem_wire_t, wdt_resets) },
    { .key = "ota_validated", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_info_telem_wire_t, ota_validated) },
    { .key = "time_valid", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_info_telem_wire_t, time_valid) },
    { .key = "bb_mem_out", .type = BB_TYPE_U64,
      .offset = offsetof(bb_info_telem_wire_t, bb_mem_out) },
    { .key = "bb_mem_peak", .type = BB_TYPE_U64,
      .offset = offsetof(bb_info_telem_wire_t, bb_mem_peak) },
    { .key = "bb_mem_fail", .type = BB_TYPE_U64,
      .offset = offsetof(bb_info_telem_wire_t, bb_mem_fail) },
};

static const bb_serialize_field_t s_info_telem_env_fields[] = {
    { .key = "ts_ms", .type = BB_TYPE_I64,
      .offset = offsetof(bb_info_telem_env_t, ts_ms) },
    { .key = "data", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_info_telem_env_t, data),
      .children = s_info_telem_data_fields, .n_children = 12 },
};

const bb_serialize_desc_t bb_info_telem_wire_desc = {
    .type_name = "info_telem_env",
    .fields    = s_info_telem_env_fields,
    .n_fields  = 2,
    .snap_size = sizeof(bb_info_telem_env_t),
};

#pragma once

// Private: v2 wire descriptor for the InfoTelemetry envelope
// {"ts_ms":N,"data":{...}} (B1-767 PR-6).
//
// bb_serialize_desc_t SSOT reproducing TODAY's on-wire bytes byte-for-byte
// (see test/test_host/test_v2_golden.c). ADDITIVE only -- not yet wired into
// the live bb_cache envelope path (platform/espidf/bb_cache/bb_cache_espidf.c
// bb_cache_post() / platform/host/bb_pub_info/bb_pub_info.c info_serialize()),
// which still emits via bb_json_t directly. The deferred cutover will serve
// this descriptor instead.
//
// Integer fields are typed BB_TYPE_I64/BB_TYPE_U64, NOT BB_TYPE_F64: today's
// serializer renders them via cJSON bb_json_obj_set_number((double)x), which
// cJSON prints as a bare integer -- bb_serialize_json's F64 backend would
// instead emit a fixed 6-decimal float, which would NOT match today's bytes.
//
// ota_ready is intentionally OMITTED (see bb_info_wire_priv.h -- same
// gate/rationale, BB_PUB_INFO_EMIT_OTA_READY).
//
// bb_info_telem_wire_t is a HAND-MAINTAINED PARALLEL of the live
// bb_info_snap_t (platform/host/bb_pub_info/bb_pub_info.c) -- there is no
// compile-time link between the two struct definitions, only this comment
// and the golden/differential test. Field widths are deliberately
// normalized to uint64_t/int64_t here (bb_serialize's BB_TYPE_U64/I64
// carriers) where the live struct uses narrower host types (size_t for the
// heap/psram/bb_mem fields, uint32_t for wdt_resets/bb_mem_fail) -- same
// values, wider wire type. At cutover, unify the snapshot and the
// descriptor's struct into ONE SSOT struct (bb_info_snap_t itself
// described by a bb_serialize_desc_t) to eliminate this drift risk instead
// of maintaining two struct definitions in parallel.

#include "bb_serialize.h"

#include <stdbool.h>
#include <stdint.h>

// data subsection -- field order matches info_serialize()
// (platform/host/bb_pub_info/bb_pub_info.c).
typedef struct {
    uint64_t heap_internal_free;
    uint64_t heap_internal_total;
    uint64_t heap_internal_largest_block;
    uint64_t heap_internal_min_free;
    uint64_t psram_free;   // present == has_psram
    uint64_t psram_total;  // present == has_psram
    bool     has_psram;    // not itself emitted -- gates psram_free/psram_total
    uint64_t wdt_resets;
    bool     ota_validated;
    bool     time_valid;
    uint64_t bb_mem_out;
    uint64_t bb_mem_peak;
    uint64_t bb_mem_fail;
} bb_info_telem_wire_t;

// Envelope root -- bb_cache owns ts_ms (B1-570 PR-3). Field order: ts_ms
// FIRST, then the nested "data" object.
typedef struct {
    int64_t               ts_ms;
    bb_info_telem_wire_t  data;
} bb_info_telem_env_t;

extern const bb_serialize_desc_t bb_info_telem_wire_desc;

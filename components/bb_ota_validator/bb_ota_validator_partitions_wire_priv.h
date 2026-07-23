#pragma once

// bb_ota_validator_partitions_wire — private wire descriptor (SSOT) for the
// GET /api/update/partitions response. Response body is a top-level object
// with one array field: {"partitions":[{label,address,size,running,state},
// ...]} — migration of partitions_handler's bb_json_t-based emitter to a
// bb_serialize descriptor, mirroring bb_diag_storage_partitions.h's
// BB_TYPE_ARR + BB_ARR_FIXED (elem_type == BB_TYPE_OBJ) wiring and
// bb_wifi_http_scan_wire_priv.h's object-wrap precedent. Portable: no
// ESP-IDF/FreeRTOS types, compiles on host + ESP-IDF.
//
// Distinct from bb_diag_storage_partitions_desc: that section lists ALL
// partitions (app + data + ...) for the storage-inventory diag cluster and
// has no `running`/`state` fields. This descriptor is scoped to APP
// partitions only (esp_partition_find(ESP_PARTITION_TYPE_APP, ...ANY)) and
// carries the OTA-slot-specific `running`/`state` fields the update-status
// route needs.
//
// Included by:
//   - platform/espidf/bb_ota_validator/bb_ota_validator.c (the live handler)
//   - test/test_host/test_ota_validator_partitions_wire.c (expected-JSON
//     fixtures)

#include "bb_serialize.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capacity (Kconfig bridge -- pattern from bb_diag_storage_partitions.h)
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#ifdef CONFIG_BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP
#define BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP CONFIG_BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP
#endif
#ifndef BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP
// 18 = the ESP-IDF APP-subtype maximum (factory + ota_0..ota_15 + test) --
// a valid partition table can never exceed this, so the default can never
// silently truncate.
#define BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP 18
#endif

// Staging row read from a live esp_partition_t -- src-only, never wire-
// emitted directly. `label`/`state` are BORROWED pointers (label points at
// the esp_partition_t's own label buffer, state at ota_state_str()'s static
// string) -- both must outlive the bb_ota_validator_partitions_wire_copy_rows()
// call that reads them.
typedef struct {
    const char *label;
    uint64_t    address;
    uint64_t    size;
    bool        running;
    const char *state;
} bb_ota_validator_partition_src_t;

// One app-partition row in the GET /api/update/partitions response's
// "partitions" array. address/size are widened to uint64_t -- same
// bb_serialize_walk() fixed-8-byte BB_TYPE_U64 constraint documented in
// bb_partition_serialize.h (the source esp_partition_t fields are
// uint32_t). `label` is COPIED into a fixed buffer sized to
// esp_partition_t.label's own bound (16 + NUL); `state` is a BORROWED
// bb_serialize_str_n_t pointing at ota_state_str()'s static string -- same
// convention as bb_wifi_http_info_wire_t's disc_reason field.
typedef struct {
    char                  label[17];
    uint64_t              address;
    uint64_t              size;
    bool                  running;
    bb_serialize_str_n_t  state;
} bb_ota_validator_partition_wire_t;

// Row descriptor (5 fields) -- shared by the production handler and the
// host tests.
extern const bb_serialize_field_t bb_ota_validator_partition_wire_fields[5];
// SSOT field count, computed from the array above -- mirrors
// bb_wifi_http_scan_ap_wire_n_fields's pattern. Callers pass this, never a
// hand-typed literal, so the count can never desync from the array.
extern const uint16_t             bb_ota_validator_partition_wire_n_fields;

// Top-level object snapshot: `partitions_items` is the backing row storage,
// `partitions` is the bb_serialize_arr_t carrier the descriptor's
// "partitions" BB_TYPE_ARR field points at -- same storage/carrier split as
// bb_wifi_http_scan_wire_t's aps_items/aps.
typedef struct {
    bb_ota_validator_partition_wire_t partitions_items[BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP];
    bb_serialize_arr_t                partitions;
} bb_ota_validator_partitions_wire_t;

// Top-level object descriptor: one field, key "partitions", BB_TYPE_ARR of
// BB_TYPE_OBJ rows (bb_ota_validator_partition_wire_fields), BB_ARR_FIXED
// (default) cardinality. Renders {"partitions":[...]} via
// bb_http_serialize_stream()/bb_serialize_json_render().
extern const bb_serialize_desc_t bb_ota_validator_partitions_wire_desc;

// bb_serialize_desc_meta_t companion (B1-1059 PR-2b-i-1) -- co-located JSON
// Schema docs/validation table for bb_ota_validator_partitions_wire_desc
// above, same #if-gated pattern as bb_wifi_http_wire_priv.h's exemplar
// (B1-1059 PR-2a). BB_SERIALIZE_META_HOST is a host-only define (set by the
// PlatformIO native env; see platformio.ini) -- NEVER set by the ESP-IDF/
// device build, so this declaration (and its definition in
// bb_ota_validator_partitions_wire.c) compiles to nothing on-device.
#if defined(BB_SERIALIZE_META_HOST)
#include "bb_serialize_meta.h"

extern const bb_serialize_desc_meta_t bb_ota_validator_partitions_wire_meta;
#endif /* BB_SERIALIZE_META_HOST */

// Pure row-copy helper: copies `n` rows from `src` into `dst` (row-count
// bounded by BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP by the caller). Copies
// `label` into the fixed buffer (strncpy + explicit terminate) and wires
// `state`'s bb_serialize_str_n_t to the src row's borrowed static string.
// Host-testable without a live partition table -- the sole reason this is
// factored out of bb_ota_validator_partitions_wire_fill() below. `n` MUST
// be <= BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP -- the caller (both
// bb_ota_validator_partitions_wire_fill() and the host tests) is the only
// source of that bound, this helper does not itself clamp it.
void bb_ota_validator_partitions_wire_copy_rows(bb_ota_validator_partition_wire_t *dst,
                                                 const bb_ota_validator_partition_src_t *src,
                                                 size_t n);

#ifdef ESP_PLATFORM
// Fills `dst` from the live app-partition table: zero-inits `dst`
// (defensive), iterates esp_partition_find(ESP_PARTITION_TYPE_APP,
// ESP_PARTITION_SUBTYPE_ANY, NULL) into a stack staging array (resolving
// `running` against esp_ota_get_running_partition() and `state` via
// ota_state_str(esp_ota_get_state_partition(p))) bounded to
// [0, BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP] by the iteration loop itself,
// copies via bb_ota_validator_partitions_wire_copy_rows() above, and wires
// `dst->partitions` (items/count) to point at the freshly-filled
// `dst->partitions_items`.
void bb_ota_validator_partitions_wire_fill(bb_ota_validator_partitions_wire_t *dst);
#endif

#ifdef __cplusplus
}
#endif

// bb_ota_validator_partitions_wire — wire descriptor + fill for the GET
// /api/update/partitions response. See
// bb_ota_validator_partitions_wire_priv.h for the {"partitions":[...]}
// shape this migration produces.

#include "bb_ota_validator_partitions_wire_priv.h"

#include <stddef.h>
#include <string.h>

const bb_serialize_field_t bb_ota_validator_partition_wire_fields[5] = {
    { .key = "label", .type = BB_TYPE_STR,
      .offset = offsetof(bb_ota_validator_partition_wire_t, label),
      .max_len = sizeof(((bb_ota_validator_partition_wire_t *)0)->label) },
    { .key = "address", .type = BB_TYPE_U64,
      .offset = offsetof(bb_ota_validator_partition_wire_t, address) },
    { .key = "size", .type = BB_TYPE_U64,
      .offset = offsetof(bb_ota_validator_partition_wire_t, size) },
    { .key = "running", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_ota_validator_partition_wire_t, running) },
    { .key = "state", .type = BB_TYPE_STR_N,
      .offset = offsetof(bb_ota_validator_partition_wire_t, state) },
};

const uint16_t bb_ota_validator_partition_wire_n_fields =
    sizeof(bb_ota_validator_partition_wire_fields) / sizeof(bb_ota_validator_partition_wire_fields[0]);

// bb_ota_validator_partition_wire_n_fields (above) is a `const uint16_t`,
// not a constant expression -- it can't initialize `.n_children` even in
// this same TU. The literal below encodes the documented 5-field row shape
// above; a drift is caught by
// test_ota_validator_partitions_wire_row_field_count_matches asserting
// bb_ota_validator_partition_wire_n_fields == 5 against the SAME extern the
// runtime count comes from (mirrors bb_wifi_http_scan_wire.c's precedent).
#define BB_OTA_VALIDATOR_PARTITION_ROW_N_FIELDS 5

static const bb_serialize_field_t s_partitions_wire_fields[1] = {
    { .key = "partitions", .type = BB_TYPE_ARR,
      .offset = offsetof(bb_ota_validator_partitions_wire_t, partitions),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(bb_ota_validator_partition_wire_t),
      .max_items = BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP,
      .children = bb_ota_validator_partition_wire_fields,
      .n_children = BB_OTA_VALIDATOR_PARTITION_ROW_N_FIELDS },
};

const bb_serialize_desc_t bb_ota_validator_partitions_wire_desc = {
    .type_name = "bb_ota_validator_partitions_wire_t",
    .fields    = s_partitions_wire_fields,
    .n_fields  = sizeof(s_partitions_wire_fields) / sizeof(s_partitions_wire_fields[0]),
    .snap_size = sizeof(bb_ota_validator_partitions_wire_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1059 PR-2b-i-1) -- co-located JSON Schema
// companion to bb_ota_validator_partitions_wire_desc above, gated behind
// BB_SERIALIZE_META_HOST (see bb_ota_validator_partitions_wire_priv.h's
// banner). Byte-fidelity vs the hand-authored
// platform/espidf/bb_ota_validator/bb_ota_validator.c's s_partitions_responses[]
// literal: see test_bb_ota_validator_partitions_wire_meta_golden.c for the
// full comparison + documented deltas (top-level additionalProperties:false,
// and the nested items object's "required" list, which the composer never
// emits for BB_TYPE_ARR-of-BB_TYPE_OBJ fields).
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_HOST)

static const bb_serialize_field_meta_t s_partition_row_wire_meta_rows[] = {
    { .key = "label",   .required = true },
    { .key = "address", .required = true },
    { .key = "size",    .required = true },
    { .key = "running", .required = true },
    { .key = "state",   .required = true },
};

static const bb_serialize_field_meta_t s_partitions_wire_meta_rows[] = {
    { .key = "partitions", .required = true,
      .children = s_partition_row_wire_meta_rows,
      .n_children = sizeof(s_partition_row_wire_meta_rows) / sizeof(s_partition_row_wire_meta_rows[0]) },
};

const bb_serialize_desc_meta_t bb_ota_validator_partitions_wire_meta = {
    .type_name = "bb_ota_validator_partitions_wire_t",
    .rows      = s_partitions_wire_meta_rows,
    .n_rows    = sizeof(s_partitions_wire_meta_rows) / sizeof(s_partitions_wire_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_HOST */

void bb_ota_validator_partitions_wire_copy_rows(bb_ota_validator_partition_wire_t *dst,
                                                 const bb_ota_validator_partition_src_t *src,
                                                 size_t n)
{
    for (size_t i = 0; i < n; i++) {
        strncpy(dst[i].label, src[i].label ? src[i].label : "", sizeof(dst[i].label) - 1);
        dst[i].label[sizeof(dst[i].label) - 1] = '\0';
        dst[i].address = src[i].address;
        dst[i].size = src[i].size;
        dst[i].running = src[i].running;
        const char *state = src[i].state ? src[i].state : "";
        dst[i].state = (bb_serialize_str_n_t){ .ptr = state, .len = strlen(state) };
    }
}

#ifdef ESP_PLATFORM

#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "bb_log.h"

static const char *TAG = "bb_ota_val_part_wire";

static const char *ota_state_str(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:      return "undefined";
    default:                         return "undefined";
    }
}

void bb_ota_validator_partitions_wire_fill(bb_ota_validator_partitions_wire_t *dst)
{
    memset(dst, 0, sizeof(*dst));

    const esp_partition_t *running = esp_ota_get_running_partition();

    bb_ota_validator_partition_src_t src[BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP];
    size_t n = 0;

    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                      ESP_PARTITION_SUBTYPE_ANY,
                                                      NULL);
    while (it && n < BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p) {
            esp_ota_img_states_t st;
            const char *state_str = "undefined";
            if (esp_ota_get_state_partition(p, &st) == ESP_OK) {
                state_str = ota_state_str(st);
            }

            src[n].label   = p->label;
            src[n].address = (uint64_t)p->address;
            src[n].size    = (uint64_t)p->size;
            src[n].running = (p == running);
            src[n].state   = state_str;
            n++;
        }
        it = esp_partition_next(it);
    }
    if (n == BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP && it != NULL) {
        // Cap was hit and the iterator wasn't exhausted -- more app
        // partitions exist than the row cap can carry. Only reachable if
        // BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP is lowered below the real
        // app-partition count (default 18 covers every valid ESP-IDF
        // partition table). Diagnostic only -- the response still emits
        // the first `n` rows.
        bb_log_w(TAG, "GET /api/update/partitions: row cap (%u) hit, "
                      "remaining app partitions truncated",
                 (unsigned)BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP);
    }
    esp_partition_iterator_release(it);

    // n is bounded to [0, BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP] by the loop
    // condition above -- unlike bb_wifi_http_scan_wire_fill()'s int-returning
    // bb_wifi_scan_get_cached() source, there is no external int-to-size_t
    // narrowing here to clamp: n is size_t from zero-init and the while
    // condition itself is the bound, so a post-loop clamp would be dead
    // code. bb_ota_validator_partitions_wire_copy_rows() below is the
    // host-testable seam this fn defers to, same rationale as the scan-wire
    // precedent.
    bb_ota_validator_partitions_wire_copy_rows(dst->partitions_items, src, n);
    dst->partitions.items = dst->partitions_items;
    dst->partitions.count = n;
}

#endif /* ESP_PLATFORM */

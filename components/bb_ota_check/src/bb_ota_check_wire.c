// bb_ota_check_wire — the format-agnostic "update.available" descriptor
// SSOT + gather. See bb_ota_check_wire.h for the wire-struct contract.
// Compiles on both host and ESP-IDF; no platform-specific code
// (bb_cache_get_raw() is itself portable).

#include "bb_ota_check_wire.h"

#include "bb_cache.h"

#include <stddef.h>
#include <string.h>

static bool ota_check_last_check_ts_present(const void *snap)
{
    const bb_ota_check_snap_t *s = (const bb_ota_check_snap_t *)snap;
    return s->last_check_ts != 0;
}

static const bb_serialize_field_t s_ota_check_wire_fields[] = {
    { .key = "current", .type = BB_TYPE_STR,
      .offset = offsetof(bb_ota_check_snap_t, current),
      .max_len = sizeof(((bb_ota_check_snap_t *)0)->current) },
    { .key = "latest", .type = BB_TYPE_STR,
      .offset = offsetof(bb_ota_check_snap_t, latest),
      .max_len = sizeof(((bb_ota_check_snap_t *)0)->latest) },
    { .key = "download_url", .type = BB_TYPE_STR,
      .offset = offsetof(bb_ota_check_snap_t, download_url),
      .max_len = sizeof(((bb_ota_check_snap_t *)0)->download_url) },
    { .key = "available", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_ota_check_snap_t, available) },
    { .key = "ts", .type = BB_TYPE_I64,
      .offset = offsetof(bb_ota_check_snap_t, ts) },
    { .key = "last_check_ok", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_ota_check_snap_t, last_check_ok) },
    { .key = "enabled", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_ota_check_snap_t, enabled) },
    { .key = "outcome", .type = BB_TYPE_STR,
      .offset = offsetof(bb_ota_check_snap_t, outcome),
      .max_len = sizeof(((bb_ota_check_snap_t *)0)->outcome) },
    { .key = "last_check_ts", .type = BB_TYPE_I64,
      .offset = offsetof(bb_ota_check_snap_t, last_check_ts),
      .present = ota_check_last_check_ts_present },
};

const bb_serialize_desc_t bb_ota_check_wire_desc = {
    .type_name = "update_available",
    .fields    = s_ota_check_wire_fields,
    .n_fields  = sizeof(s_ota_check_wire_fields) / sizeof(s_ota_check_wire_fields[0]),
    .snap_size = sizeof(bb_ota_check_snap_t),
};

bb_err_t bb_ota_check_gather(bb_ota_check_snap_t *dst)
{
    if (!dst) return BB_ERR_INVALID_ARG;
    memset(dst, 0, sizeof(*dst));
    return bb_cache_get_raw(BB_OTA_CHECK_TOPIC, dst, sizeof(*dst));
}

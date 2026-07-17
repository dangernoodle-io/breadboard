// bb_system_snap -- the format-agnostic system-snapshot descriptor SSOT,
// owned by bb_system. See bb_system_snap.h for the snapshot-struct contract.
// Compiles on both host and ESP-IDF; no platform-specific code
// (bb_system_snap_fill() draws only from bb_system's already-portable
// accessors). ADDITIVE only -- not yet consumed by any route/serializer.

#include "../bb_system_snap.h"

#include "bb_system.h"
#include "bb_str.h"

#include <stddef.h>

static const bb_serialize_field_t s_system_snap_fields[] = {
    { .key = "version", .type = BB_TYPE_STR,
      .offset = offsetof(bb_system_snap_t, version), .max_len = BB_SYSTEM_SNAP_VERSION_MAX_LEN },
    { .key = "project_name", .type = BB_TYPE_STR,
      .offset = offsetof(bb_system_snap_t, project_name), .max_len = BB_SYSTEM_SNAP_PROJECT_NAME_MAX_LEN },
    { .key = "idf_version", .type = BB_TYPE_STR,
      .offset = offsetof(bb_system_snap_t, idf_version), .max_len = BB_SYSTEM_SNAP_IDF_VERSION_MAX_LEN },
    { .key = "reset_reason", .type = BB_TYPE_STR,
      .offset = offsetof(bb_system_snap_t, reset_reason), .max_len = BB_SYSTEM_SNAP_RESET_REASON_MAX_LEN },
    { .key = "boot_count", .type = BB_TYPE_U64,
      .offset = offsetof(bb_system_snap_t, boot_count) },
    { .key = "build_date", .type = BB_TYPE_STR,
      .offset = offsetof(bb_system_snap_t, build_date), .max_len = BB_SYSTEM_SNAP_BUILD_DATE_MAX_LEN },
    { .key = "build_time", .type = BB_TYPE_STR,
      .offset = offsetof(bb_system_snap_t, build_time), .max_len = BB_SYSTEM_SNAP_BUILD_TIME_MAX_LEN },
};

const bb_serialize_desc_t bb_system_snap_desc = {
    .type_name = "system",
    .fields    = s_system_snap_fields,
    .n_fields  = sizeof(s_system_snap_fields) / sizeof(s_system_snap_fields[0]),
    .snap_size = sizeof(bb_system_snap_t),
};

bb_err_t bb_system_snap_fill(bb_system_snap_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;

    bb_strlcpy(out->version, bb_system_get_version(), sizeof(out->version));
    bb_strlcpy(out->project_name, bb_system_get_project_name(), sizeof(out->project_name));
    bb_strlcpy(out->idf_version, bb_system_get_idf_version(), sizeof(out->idf_version));
    bb_strlcpy(out->reset_reason, bb_system_reset_reason_str(bb_system_get_reset_reason()),
               sizeof(out->reset_reason));
    out->boot_count = (uint64_t)bb_system_boot_count_get();
    bb_strlcpy(out->build_date, bb_system_get_build_date(), sizeof(out->build_date));
    bb_strlcpy(out->build_time, bb_system_get_build_time(), sizeof(out->build_time));

    return BB_OK;
}

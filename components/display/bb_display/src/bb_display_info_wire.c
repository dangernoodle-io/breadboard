// bb_display_info_wire — the format-agnostic "health.display" descriptor
// SSOT + gather. See bb_display_info_wire.h for the wire-struct contract.
// Compiles on both host and ESP-IDF; no platform-specific code
// (bb_cache_get_raw() is itself portable).

#include "bb_display_info_wire.h"

#include "bb_display_info_event_priv.h"
#include "bb_cache.h"
#include "bb_data.h"
#include "bb_str.h"

#include <stddef.h>
#include <string.h>

static bool display_info_present(const void *snap)
{
    const bb_display_info_wire_t *s = (const bb_display_info_wire_t *)snap;
    return s->present;
}

static const bb_serialize_field_t s_display_info_wire_fields[] = {
    { .key = "present", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_display_info_wire_t, present) },
    { .key = "panel", .type = BB_TYPE_STR,
      .offset = offsetof(bb_display_info_wire_t, panel),
      .max_len = sizeof(((bb_display_info_wire_t *)0)->panel),
      .present = display_info_present },
    { .key = "width", .type = BB_TYPE_I64,
      .offset = offsetof(bb_display_info_wire_t, width),
      .present = display_info_present },
    { .key = "height", .type = BB_TYPE_I64,
      .offset = offsetof(bb_display_info_wire_t, height),
      .present = display_info_present },
    { .key = "enabled", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_display_info_wire_t, enabled),
      .present = display_info_present },
};

const bb_serialize_desc_t bb_display_info_wire_desc = {
    .type_name = "health_display",
    .fields    = s_display_info_wire_fields,
    .n_fields  = sizeof(s_display_info_wire_fields) / sizeof(s_display_info_wire_fields[0]),
    .snap_size = sizeof(bb_display_info_wire_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1179) -- co-located JSON Schema companion to
// bb_display_info_wire_desc above, gated behind BB_SERIALIZE_META_HOST (see
// bb_display_info_wire.h's banner), same pattern as
// bb_diag_boot_wire.c's exemplar. "present" is the only field with no
// `.present` predicate, so it's the only row marked `.required = true`;
// "panel"/"width"/"height"/"enabled" are all gated by display_info_present()
// above (see bb_serialize_walk.c's present-predicate skip), so none is
// required -- mirroring bb_diag_boot_wire.c's `boots_since`/`detail`/
// `age_s` rows (present-gated fields never marked required). This PR also
// corrects platform/espidf/bb_display/bb_display_info.c's hand-authored
// k_display_schema literal, which used to mark "panel" a nullable
// `["string","null"]` union -- the serializer never emits a JSON null for
// an absent-present field, it OMITS the key entirely, so the corrected
// literal makes "panel" a plain (optional) string; see
// test_bb_display_info_wire_meta_golden.c for the byte-fidelity proof
// against the corrected literal.
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_SHIP)

static const bb_serialize_field_meta_t s_display_info_wire_meta_rows[] = {
    { .key = "present", .required = true },
    { .key = "panel" },
    { .key = "width" },
    { .key = "height" },
    { .key = "enabled" },
};

const bb_serialize_desc_meta_t bb_display_info_wire_meta = {
    .type_name = "health_display",
    .rows      = s_display_info_wire_meta_rows,
    .n_rows    = sizeof(s_display_info_wire_meta_rows) / sizeof(s_display_info_wire_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */

bb_err_t bb_display_info_gather(bb_display_info_wire_t *dst)
{
    if (!dst) return BB_ERR_INVALID_ARG;

    bb_display_snap_t raw;
    memset(&raw, 0, sizeof(raw));
    bb_err_t err = bb_cache_get_raw(BB_DISPLAY_INFO_TOPIC, &raw, sizeof(raw));
    if (err != BB_OK) return err;

    memset(dst, 0, sizeof(*dst));
    dst->present = raw.present;
    bb_strlcpy(dst->panel, raw.panel, sizeof(dst->panel));
    dst->width   = (int64_t)raw.width;
    dst->height  = (int64_t)raw.height;
    dst->enabled = raw.enabled;

    return BB_OK;
}

// ---------------------------------------------------------------------------
// bb_data bind (B1-1146a) -- see bb_display_info_wire.h's file header for why
// this self-bind lives here rather than at a single composition-root call
// site. No REST/SSE render seam here: health.display's REST exposure is
// being rehomed to system.display under bb_system's diag endpoint (B1-1150).
// ---------------------------------------------------------------------------

// Adapter: bb_data_gather_fn wraps bb_display_info_gather()'s typed
// signature. "health.display" has no request-scoped filter, so `args` is
// unused.
static bb_err_t display_info_data_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    return bb_display_info_gather((bb_display_info_wire_t *)dst);
}

bb_err_t bb_display_info_bind(void)
{
    bb_data_binding_t binding = {
        .key    = BB_DISPLAY_INFO_TOPIC,
        .desc   = &bb_display_info_wire_desc,
        .gather = display_info_data_gather,
    };
    return bb_data_bind(&binding);
}

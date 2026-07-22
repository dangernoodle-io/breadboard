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

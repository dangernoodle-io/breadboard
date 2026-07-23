// bb_mqtt_client -- /api/health "mqtt" section (B1-1099, PR-4 of the
// bb_health/bb_response migration chain, epic B1-1054). Portable (no
// ESP_PLATFORM gate needed): reads only bb_mqtt_client_default() +
// bb_mqtt_client_is_connected(), both already portable across backends
// (mirrors bb_mqtt_client_health.c's per-instance descriptor TU).
//
// Folds the now-deleted bb_mqtt_info component's bb_mqtt_register_health()
// producer onto the bb_health_section composer seam. See bb_mqtt_client.h
// for the wire-shape contract and the additive-and-inert note.
#include "bb_mqtt_client.h"

#include <stddef.h>

// JSON-Schema value for the "mqtt" section contributed to the /api/health
// 200 schema -- carried verbatim from bb_mqtt_info's k_mqtt_schema.
static const char k_mqtt_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"enabled\":{\"type\":\"boolean\"},"
    "\"connected\":{\"type\":\"boolean\"}}}";

static const bb_serialize_field_t s_mqtt_health_section_fields[] = {
    { .key = "enabled", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_mqtt_client_health_section_snap_t, enabled) },
    { .key = "connected", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_mqtt_client_health_section_snap_t, connected) },
};

const bb_serialize_desc_t bb_mqtt_client_health_section_desc = {
    .type_name = "mqtt_health_section",
    .fields    = s_mqtt_health_section_fields,
    .n_fields  = sizeof(s_mqtt_health_section_fields) / sizeof(s_mqtt_health_section_fields[0]),
    .snap_size = sizeof(bb_mqtt_client_health_section_snap_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1059 PR-2b-i-3) -- co-located JSON Schema
// companion to bb_mqtt_client_health_section_desc above, gated behind
// BB_SERIALIZE_META_HOST (see bb_mqtt_client.h's banner). Both fields are
// unconditionally filled by bb_mqtt_client_health_section_fill() below (no
// `.present` predicate on either), so both are marked required=true --
// k_mqtt_schema itself carries no "required" array because it's a bare
// /api/health section fragment (see test_bb_mqtt_client_health_section_meta_
// golden.c for the fragment-only fidelity proof).
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_HOST)

static const bb_serialize_field_meta_t s_mqtt_health_section_meta_rows[] = {
    { .key = "enabled",   .required = true },
    { .key = "connected", .required = true },
};

const bb_serialize_desc_meta_t bb_mqtt_client_health_section_meta = {
    .type_name = "mqtt_health_section",
    .rows      = s_mqtt_health_section_meta_rows,
    .n_rows    = sizeof(s_mqtt_health_section_meta_rows) / sizeof(s_mqtt_health_section_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_HOST */

bb_err_t bb_mqtt_client_health_section_fill(void *dst, const bb_health_fill_args_t *args)
{
    (void)args;
    if (!dst) return BB_ERR_INVALID_ARG;

    bb_mqtt_client_health_section_snap_t *snap = (bb_mqtt_client_health_section_snap_t *)dst;
    bb_mqtt_client_t h = bb_mqtt_client_default();
    snap->enabled   = (h != NULL);
    snap->connected = snap->enabled && bb_mqtt_client_is_connected(h);
    return BB_OK;
}

void bb_mqtt_client_health_register(void)
{
    bb_health_section_t section = {
        .name         = "mqtt",
        .snap_desc    = &bb_mqtt_client_health_section_desc,
        .fill         = bb_mqtt_client_health_section_fill,
        .ctx          = NULL,
        .schema_props = k_mqtt_schema,
    };
    bb_health_section_register(&section);
}

bb_err_t bb_mqtt_client_health_autoregister_init(void)
{
    bb_mqtt_client_health_register();
    return BB_OK;
}

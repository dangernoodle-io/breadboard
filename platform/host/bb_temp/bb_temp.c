#include "bb_temp.h"

#include <stddef.h>

#ifdef BB_TEMP_TESTING
#include "bb_temp_test.h"
#endif

/* JSON-Schema value for the "temp" section contributed to the /api/health 200 schema. */
static const char k_temp_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"soc_c\":{\"type\":\"number\"}}}";

#ifdef BB_TEMP_TESTING
static bool  s_test_present = false;
static float s_test_celsius = 0.0f;

void bb_temp_test_set_soc(bool present, float celsius)
{
    s_test_present = present;
    s_test_celsius = celsius;
}

bool bb_temp_read_soc(float *out_celsius)
{
    if (!out_celsius) return false;
    if (s_test_present) *out_celsius = s_test_celsius;
    return s_test_present;
}
#else
bool bb_temp_read_soc(float *out_celsius)
{
    (void)out_celsius;
    return false;
}
#endif

static bool temp_health_soc_c_present(const void *snap)
{
    return ((const bb_temp_health_snap_t *)snap)->present;
}

static const bb_serialize_field_t s_temp_health_fields[] = {
    { .key = "present", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_temp_health_snap_t, present) },
    { .key = "soc_c", .type = BB_TYPE_F64,
      .offset = offsetof(bb_temp_health_snap_t, soc_c),
      .present = temp_health_soc_c_present },
};

const bb_serialize_desc_t bb_temp_health_desc = {
    .type_name = "temp_health",
    .fields    = s_temp_health_fields,
    .n_fields  = sizeof(s_temp_health_fields) / sizeof(s_temp_health_fields[0]),
    .snap_size = sizeof(bb_temp_health_snap_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1059 PR-2b-i-3) -- co-located JSON Schema
// companion to bb_temp_health_desc above, gated behind
// BB_SERIALIZE_META_HOST (see bb_temp.h's banner). PLATFORM TWIN: kept
// byte-identical with platform/espidf/bb_temp/bb_temp.c's copy by
// convention, same as the rest of this file. `present` is unconditionally
// filled (required=true); `soc_c` is omitted via the .present predicate
// above (required=false) -- k_temp_schema itself carries no "required"
// array because it's a bare /api/health section fragment (see
// test_bb_temp_health_meta_golden.c for the fragment-only fidelity proof).
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_SHIP)

static const bb_serialize_field_meta_t s_temp_health_meta_rows[] = {
    { .key = "present", .required = true },
    { .key = "soc_c" },
};

const bb_serialize_desc_meta_t bb_temp_health_meta = {
    .type_name = "temp_health",
    .rows      = s_temp_health_meta_rows,
    .n_rows    = sizeof(s_temp_health_meta_rows) / sizeof(s_temp_health_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */

bb_err_t bb_temp_health_fill(void *dst, const bb_health_fill_args_t *args)
{
    (void)args;
    if (!dst) return BB_ERR_INVALID_ARG;

    bb_temp_health_snap_t *snap = (bb_temp_health_snap_t *)dst;
    float c = 0.0f;
    if (bb_temp_read_soc(&c)) {
        snap->present = true;
        /* Round to 1 decimal: multiply, round, divide -- same value-level
         * rounding today's emitter applies, kept independent of wire
         * formatting (B1-1098). */
        snap->soc_c = (double)((int)(c * 10.0f + 0.5f)) / 10.0;
    } else {
        snap->present = false;
        snap->soc_c   = 0.0;
    }
    return BB_OK;
}

void bb_temp_register_info(void)
{
    bb_health_section_t section = {
        .name         = "temp",
        .snap_desc    = &bb_temp_health_desc,
        .fill         = bb_temp_health_fill,
        .ctx          = NULL,
        .schema_props = k_temp_schema,
    };
    bb_health_section_register(&section);
}

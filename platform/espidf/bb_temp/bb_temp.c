#include "bb_temp.h"
#include "bb_system.h"

#include <stddef.h>

/* JSON-Schema value for the "temp" section contributed to the /api/health 200 schema. */
static const char k_temp_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"soc_c\":{\"type\":\"number\"}}}";

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

bool bb_temp_read_soc(float *out_celsius)
{
    if (!out_celsius) return false;
    return bb_system_read_temp_celsius(out_celsius) == BB_OK;
}

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

bb_err_t bb_temp_autoregister_init(void)
{
    bb_temp_register_info();
    return BB_OK;
}

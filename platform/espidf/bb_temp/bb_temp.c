#include "bb_temp.h"
#include "bb_health.h"
#include "bb_json.h"
#include "bb_system.h"

/* JSON-Schema value for the "temp" section contributed to the /api/health 200 schema. */
static const char k_temp_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"soc_c\":{\"type\":\"number\"}}}";

static void temp_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;
    bb_json_t child = section;
    float c = 0.0f;

    if (bb_temp_read_soc(&c)) {
        bb_json_obj_set_bool(child, "present", true);
        /* Round to 1 decimal: multiply, round, divide. */
        double rounded = (double)((int)(c * 10.0f + 0.5f)) / 10.0;
        bb_json_obj_set_number(child, "soc_c", rounded);
    } else {
        bb_json_obj_set_bool(child, "present", false);
    }
}

bool bb_temp_read_soc(float *out_celsius)
{
    if (!out_celsius) return false;
    return bb_system_read_temp_celsius(out_celsius) == BB_OK;
}

void bb_temp_register_info(void)
{
    bb_health_register_section("temp", temp_section_get, NULL, k_temp_schema);
}

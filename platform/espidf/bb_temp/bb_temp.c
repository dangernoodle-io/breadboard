#include "bb_temp.h"
#include "bb_health.h"
#include "bb_json.h"
#include "bb_system.h"

/* JSON-Schema properties fragment contributed to the /api/health 200 schema. */
static const char k_temp_schema_fragment[] =
    "\"temp\":{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"soc_c\":{\"type\":\"number\"}}}";

static void temp_health_extender(bb_json_t root)
{
    bb_json_t temp = bb_json_obj_new();
    float c = 0.0f;

    if (bb_temp_read_soc(&c)) {
        bb_json_obj_set_bool(temp, "present", true);
        /* Round to 1 decimal: multiply, round, divide. */
        double rounded = (double)((int)(c * 10.0f + 0.5f)) / 10.0;
        bb_json_obj_set_number(temp, "soc_c", rounded);
    } else {
        bb_json_obj_set_bool(temp, "present", false);
    }

    bb_json_obj_set_obj(root, "temp", temp);
}

bool bb_temp_read_soc(float *out_celsius)
{
    if (!out_celsius) return false;
    return bb_system_read_temp_celsius(out_celsius) == BB_OK;
}

void bb_temp_register_info(void)
{
    bb_health_register_extender_ex(temp_health_extender, k_temp_schema_fragment);
}

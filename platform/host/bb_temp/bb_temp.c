#include "bb_temp.h"
#include "bb_info.h"
#include "bb_json.h"

#ifdef BB_TEMP_TESTING
#include "bb_temp_test.h"
#endif

/* JSON-Schema properties fragment contributed to the /api/health 200 schema. */
static const char k_temp_schema_fragment[] =
    "\"temp\":{\"type\":\"object\",\"properties\":{"
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

static void temp_health_extender(void *root)
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

    bb_json_obj_set_obj((bb_json_t)root, "temp", temp);
}

void bb_temp_register_info(void)
{
    bb_health_register_extender_ex(temp_health_extender, k_temp_schema_fragment);
}

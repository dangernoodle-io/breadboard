#include "bb_thermal.h"
#include "bb_fan.h"
#include "bb_fan_test.h"
#include "bb_power.h"
#include "bb_power_test.h"
#include "bb_temp.h"
#include "bb_http_extender.h"
#include <stdbool.h>
#include <string.h>

// Host twin of the /api/thermal route — provides schema assembly and testing hooks.

static const char k_thermal_schema_base[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"soc\":{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"c\":{\"type\":[\"number\",\"null\"]}}},"
    "\"vr\":{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"c\":{\"type\":[\"number\",\"null\"]}}},"
    "\"asic\":{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"c\":{\"type\":[\"number\",\"null\"]}}},"
    "\"board\":{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"c\":{\"type\":[\"number\",\"null\"]}}}";

static const char k_thermal_schema_suffix[] =
    "},"
    "\"required\":[\"soc\",\"vr\",\"asic\",\"board\"]}";

#ifdef BB_THERMAL_TESTING

const char *bb_thermal_get_assembled_schema(void)
{
    const char *cached = bb_http_extender_get_assembled_schema("thermal");
    if (cached) return cached;
    return bb_http_route_assemble_schema("thermal", k_thermal_schema_base, k_thermal_schema_suffix);
}

void bb_thermal_reset_for_test(void)
{
    bb_temp_test_set_soc(false, 0.0f);
    bb_fan_test_reset();
    bb_power_test_reset();
    // extender reset is handled globally by bb_info_reset_for_test → bb_http_extender_reset_for_test
}

#endif /* BB_THERMAL_TESTING */

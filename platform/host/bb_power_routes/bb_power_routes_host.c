#include "bb_power_routes.h"
#include "bb_power.h"
#include "bb_http_extender.h"
#include <stdbool.h>
#include <string.h>

// Host twin of the /api/power route — provides schema assembly and testing hooks.

static const char k_power_schema_base[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"vout_mv\":{\"type\":[\"integer\",\"null\"]},"
    "\"iout_ma\":{\"type\":[\"integer\",\"null\"]},"
    "\"pout_mw\":{\"type\":[\"integer\",\"null\"]},"
    "\"vin_mv\":{\"type\":[\"integer\",\"null\"]},"
    "\"temp_c\":{\"type\":[\"integer\",\"null\"]}";

static const char k_power_schema_suffix[] =
    "},"
    "\"required\":[\"present\"]}";

#ifdef BB_POWER_ROUTES_TESTING

const char *bb_power_routes_get_assembled_schema(void)
{
    const char *cached = bb_http_extender_get_assembled_schema("power");
    if (cached) return cached;
    return bb_http_route_assemble_schema("power", k_power_schema_base, k_power_schema_suffix);
}

void bb_power_routes_reset_for_test(void)
{
    bb_power_test_reset();
    // extender reset is handled globally by bb_info_reset_for_test → bb_http_extender_reset_for_test
}

#endif /* BB_POWER_ROUTES_TESTING */

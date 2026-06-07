#include "bb_fan_routes.h"
#include "bb_fan.h"
#include "bb_http_extender.h"
#include <stdbool.h>
#include <string.h>

// Host twin of the /api/fan route — provides schema assembly and testing hooks.

static const char k_fan_schema_base[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"rpm\":{\"type\":[\"integer\",\"null\"]},"
    "\"duty_pct\":{\"type\":[\"integer\",\"null\"]}";

static const char k_fan_schema_suffix[] =
    "},"
    "\"required\":[\"present\"]}";

#ifdef BB_FAN_ROUTES_TESTING

const char *bb_fan_routes_get_assembled_schema(void)
{
    const char *cached = bb_http_extender_get_assembled_schema("fan");
    if (cached) return cached;
    return bb_http_route_assemble_schema("fan", k_fan_schema_base, k_fan_schema_suffix);
}

void bb_fan_routes_reset_for_test(void)
{
    bb_fan_test_reset();
    // extender reset is handled globally by bb_info_reset_for_test → bb_http_extender_reset_for_test
}

#endif /* BB_FAN_ROUTES_TESTING */

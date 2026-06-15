#include "bb_power_routes.h"
#include "bb_power.h"
#include "bb_http_extender.h"
#include "bb_json.h"
#include <stdbool.h>
#include <string.h>

// Host twin of the /api/power route — provides schema assembly and testing hooks.

// ---------------------------------------------------------------------------
// Shared emit helper — host implementation mirrors espidf (SSOT).
// ---------------------------------------------------------------------------

void bb_power_emit_section(bb_json_t obj)
{
    bb_power_handle_t h = bb_power_primary();
    bool present = (h != NULL);

    bb_power_snapshot_t snap;
    bb_power_snapshot(h, &snap);

    bb_json_obj_set_bool(obj, "present", present);

    if (present && snap.vout_mv >= 0) {
        bb_json_obj_set_number(obj, "vout_mv", (double)snap.vout_mv);
    } else {
        bb_json_obj_set_null(obj, "vout_mv");
    }

    if (present && snap.iout_ma >= 0) {
        bb_json_obj_set_number(obj, "iout_ma", (double)snap.iout_ma);
    } else {
        bb_json_obj_set_null(obj, "iout_ma");
    }

    if (present && snap.pout_mw >= 0) {
        bb_json_obj_set_number(obj, "pout_mw", (double)snap.pout_mw);
    } else {
        bb_json_obj_set_null(obj, "pout_mw");
    }

    if (present && snap.vin_mv >= 0) {
        bb_json_obj_set_number(obj, "vin_mv", (double)snap.vin_mv);
    } else {
        bb_json_obj_set_null(obj, "vin_mv");
    }

    if (present && snap.temp_c >= 0) {
        bb_json_obj_set_number(obj, "temp_c", (double)snap.temp_c);
    } else {
        bb_json_obj_set_null(obj, "temp_c");
    }
}

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

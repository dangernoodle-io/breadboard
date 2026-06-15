#include "bb_power_routes.h"
#include "bb_power.h"
#include "bb_json.h"
#include <stdbool.h>
#include <string.h>

// Host twin of the bb_power_routes component — emit helper + testing hooks.
// /api/power route deleted in B1-269 PR7; bb_power_emit_section remains for
// /api/sensors power section (SSOT).

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

#ifdef BB_POWER_ROUTES_TESTING

void bb_power_routes_reset_for_test(void)
{
    bb_power_test_reset();
}

#endif /* BB_POWER_ROUTES_TESTING */

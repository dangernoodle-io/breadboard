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
    bb_power_snapshot_t snap;
    bb_power_snapshot(h, &snap);
    bb_json_obj_set_bool(obj, "present", h != NULL);
    bb_power_emit(obj, &snap);
}

#ifdef BB_POWER_ROUTES_TESTING

void bb_power_routes_reset_for_test(void)
{
    bb_power_test_reset();
}

#endif /* BB_POWER_ROUTES_TESTING */

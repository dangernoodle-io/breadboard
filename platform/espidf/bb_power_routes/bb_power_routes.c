#include "bb_power_routes.h"
#include "bb_power.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_http_server.h"
#include <stdbool.h>
#include <stddef.h>

static const char *TAG __attribute__((unused)) = "bb_power_routes";

// ---------------------------------------------------------------------------
// Shared emit helper — writes power fields into an existing bb_json_t object.
// Used by /api/sensors power section (SSOT).
// ---------------------------------------------------------------------------

void bb_power_emit_section(bb_json_t obj)
{
    bb_power_handle_t h = bb_power_primary();
    bb_power_snapshot_t snap;
    bb_power_snapshot(h, &snap);
    bb_json_obj_set_bool(obj, "present", h != NULL);
    bb_power_emit(obj, &snap);
}

// ---------------------------------------------------------------------------
// bb_power_routes_init — stub; /api/power route deleted in B1-269 PR7.
// Kept so consumers that previously called this still link.
// ---------------------------------------------------------------------------

bb_err_t bb_power_routes_init(bb_http_handle_t server)
{
    (void)server;
    return BB_OK;
}

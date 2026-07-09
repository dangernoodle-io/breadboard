#include "bb_fan_routes.h"
#include "bb_fan.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_http_server.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const char *TAG __attribute__((unused)) = "bb_fan_routes";

#ifdef CONFIG_BB_FAN_AUTOFAN
// Persist callback registered by the consumer (e.g. TM) to save config to NVS.
// NOTE: POST /api/fan is deleted; this callback is kept for consumers that call
// bb_fan_set_autofan directly and want the persist hook.
static void (*s_persist_cb)(void *ctx, const bb_fan_autofan_cfg_t *cfg) = NULL;
static void *s_persist_ctx = NULL;

void bb_fan_routes_set_autofan_persist_cb(
    void (*cb)(void *ctx, const bb_fan_autofan_cfg_t *cfg), void *ctx)
{
    s_persist_cb  = cb;
    s_persist_ctx = ctx;
}
#endif /* CONFIG_BB_FAN_AUTOFAN */

// ---------------------------------------------------------------------------
// Shared emit helper — writes fan fields into an existing bb_json_t object.
// Used by /api/sensors fan section (SSOT).
// ---------------------------------------------------------------------------

void bb_fan_emit_section(bb_json_t obj)
{
    bb_fan_handle_t h = bb_fan_primary();
    bool present = (h != NULL);
    bb_fan_snapshot_t snap;
    bb_fan_snapshot(h, &snap);
    bb_json_obj_set_bool(obj, "present", present);
#ifndef CONFIG_BB_FAN_AUTOFAN
    bb_fan_emit(obj, &snap);
#else
    bb_fan_autofan_telemetry_t tel;
    bb_fan_get_autofan_telemetry(h, &tel);
    bb_fan_emit(obj, &snap, &tel);
#endif
}

// ---------------------------------------------------------------------------
// bb_fan_routes_init — stub; /api/fan route deleted in B1-269 PR7.
// Kept so consumers that previously called this still link.
// ---------------------------------------------------------------------------

bb_err_t bb_fan_routes_init(bb_http_handle_t server)
{
    (void)server;
    return BB_OK;
}

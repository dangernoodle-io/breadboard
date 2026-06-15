#include "bb_power_routes.h"
#include "bb_power.h"
#include "bb_http.h"
#include "bb_http_extender.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <stdbool.h>
#include <stddef.h>

static const char *TAG = "bb_power_routes";

// ---------------------------------------------------------------------------
// Base JSON-Schema for GET /api/power 200 response
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Route descriptor — responses[0].schema filled at init
// ---------------------------------------------------------------------------

static bb_route_response_t s_power_responses[] = {
    { 200, "application/json",
      NULL,  // filled by bb_http_route_assemble_schema() at init
      "voltage regulator power readings" },
    { 0 },
};

static const bb_route_t s_power_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/power",
    .tag       = "power",
    .summary   = "Get VR power readings",
    .responses = s_power_responses,
    .handler   = NULL,  // set below at init (can't take address of static fn here)
};

// ---------------------------------------------------------------------------
// Shared emit helper — writes power fields into an existing bb_json_t object.
// Used by /api/power GET handler and /api/sensors power section (SSOT).
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

// ---------------------------------------------------------------------------
// Emit helper — host-testable seam
// ---------------------------------------------------------------------------

void bb_power_routes_emit(bb_http_request_t *req)
{
    bb_json_t root = bb_json_obj_new();

    // Use shared emit helper (SSOT — same fields as /api/sensors power section).
    bb_power_emit_section(root);

    bb_http_route_run_extenders("power", root);

    char *str = bb_json_serialize(root);
    bb_json_free(root);
    if (!str) {
        bb_http_resp_send_chunk(req, NULL, 0);
        return;
    }
    bb_http_resp_set_type(req, "application/json");
    bb_http_resp_send_chunk(req, str, -1);
    bb_http_resp_send_chunk(req, NULL, 0);
    free(str);
}

static bb_err_t power_handler(bb_http_request_t *req)
{
    bb_power_routes_emit(req);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Init (regular-tier, order 1)
// ---------------------------------------------------------------------------

bb_err_t bb_power_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    // Assemble and publish schema. Freeze is handled globally by bb_info_init
    // (order 2). Our init runs at order 1, after order-0 satellites have
    // registered their "power" extenders but before bb_info_init freezes.
    // bb_http_route_assemble_schema itself is idempotent (caches the result).
    const char *schema = bb_http_route_assemble_schema(
        "power", k_power_schema_base, k_power_schema_suffix);
    if (!schema) {
        bb_log_w(TAG, "power schema assembly: malloc failed; schema will be NULL");
    }
    s_power_responses[0].schema = schema;

    // Wire handler into the route descriptor.
    // static: registry stores this pointer; descriptor must outlive init (B 405-walk deref)
    static bb_route_t route;
    route = s_power_route;
    route.handler = power_handler;

    bb_err_t err = bb_http_register_described_route(server, &route);
    if (err != BB_OK) return err;
    bb_log_i(TAG, "power route registered");
    return BB_OK;
}

#if CONFIG_BB_POWER_ROUTES_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_power_routes, bb_power_routes_init, 1);
#endif

#include "bb_thermal.h"
#include "bb_fan.h"
#include "bb_power.h"
#include "bb_temp.h"
#include "bb_http.h"
#include "bb_http_extender.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <math.h>
#include <stdbool.h>
#include <stddef.h>

static const char *TAG = "bb_thermal";

// ---------------------------------------------------------------------------
// Base JSON-Schema for GET /api/thermal 200 response
//
// Each source is a {present, c} object: c is number|null.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Route descriptor — responses[0].schema filled at init
// ---------------------------------------------------------------------------

static bb_route_response_t s_thermal_responses[] = {
    { 200, "application/json",
      NULL,  // filled by bb_http_route_assemble_schema() at init
      "aggregate thermal readings (soc/vr/asic/board)" },
    { 0 },
};

static const bb_route_t s_thermal_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/thermal",
    .tag       = "thermal",
    .summary   = "Get aggregate thermal readings",
    .responses = s_thermal_responses,
    .handler   = NULL,  // set below at init
};

// ---------------------------------------------------------------------------
// Emit helper — builds a {present, c} sub-object
// ---------------------------------------------------------------------------

static void emit_source(bb_json_t root, const char *key, bool present, double c_val)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_bool(obj, "present", present);
    if (present) {
        bb_json_obj_set_number(obj, "c", c_val);
    } else {
        bb_json_obj_set_null(obj, "c");
    }
    bb_json_obj_set_obj(root, key, obj);
}

// ---------------------------------------------------------------------------
// GET handler
// ---------------------------------------------------------------------------

void bb_thermal_emit(bb_http_request_t *req)
{
    // --- SoC (bb_temp) ---
    float soc_c = 0.0f;
    bool soc_present = bb_temp_read_soc(&soc_c);

    // --- VR (bb_power) ---
    bb_power_handle_t pwr = bb_power_primary();
    bb_power_snapshot_t psnap;
    bb_power_snapshot(pwr, &psnap);
    bool vr_present = (pwr != NULL && psnap.temp_c >= 0);

    // --- Fan: ASIC die + board (bb_fan) ---
    bb_fan_handle_t fan = bb_fan_primary();
    bb_fan_snapshot_t fsnap;
    bb_fan_snapshot(fan, &fsnap);
    bool asic_present  = (fan != NULL && !isnan(fsnap.die_c));
    bool board_present = (fan != NULL && !isnan(fsnap.board_c));

    bb_json_t root = bb_json_obj_new();

    emit_source(root, "soc",   soc_present,  soc_present  ? (double)soc_c       : 0.0);
    emit_source(root, "vr",    vr_present,   vr_present   ? (double)psnap.temp_c : 0.0);
    emit_source(root, "asic",  asic_present, asic_present  ? (double)fsnap.die_c  : 0.0);
    emit_source(root, "board", board_present,board_present ? (double)fsnap.board_c : 0.0);

    bb_http_route_run_extenders("thermal", root);

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

static bb_err_t thermal_handler(bb_http_request_t *req)
{
    bb_thermal_emit(req);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Init (regular-tier, order 1)
// ---------------------------------------------------------------------------

bb_err_t bb_thermal_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    const char *schema = bb_http_route_assemble_schema(
        "thermal", k_thermal_schema_base, k_thermal_schema_suffix);
    if (!schema) {
        bb_log_w(TAG, "thermal schema assembly: malloc failed; schema will be NULL");
    }
    s_thermal_responses[0].schema = schema;

    // static: registry stores this pointer; descriptor must outlive init (B 405-walk deref)
    static bb_route_t route;
    route = s_thermal_route;
    route.handler = thermal_handler;

    bb_err_t err = bb_http_register_described_route(server, &route);
    if (err != BB_OK) return err;
    bb_log_i(TAG, "thermal route registered");
    return BB_OK;
}

#if CONFIG_BB_THERMAL_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_thermal, bb_thermal_init, 1);
#endif

#include "bb_fan_routes.h"
#include "bb_fan.h"
#include "bb_http.h"
#include "bb_http_extender.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <stdbool.h>
#include <stddef.h>

static const char *TAG = "bb_fan_routes";

// ---------------------------------------------------------------------------
// Base JSON-Schema for GET /api/fan 200 response
// ---------------------------------------------------------------------------

static const char k_fan_schema_base[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"rpm\":{\"type\":[\"integer\",\"null\"]},"
    "\"duty_pct\":{\"type\":[\"integer\",\"null\"]}";

static const char k_fan_schema_suffix[] =
    "},"
    "\"required\":[\"present\"]}";

// ---------------------------------------------------------------------------
// Route descriptors — responses[0].schema filled at init
// ---------------------------------------------------------------------------

static bb_route_response_t s_fan_get_responses[] = {
    { 200, "application/json",
      NULL,  // filled by bb_http_route_assemble_schema() at init
      "fan controller readings" },
    { 0 },
};

static const bb_route_response_t s_fan_post_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"status\":{\"type\":\"string\"},"
      "\"duty_pct\":{\"type\":\"integer\"}},"
      "\"required\":[\"status\",\"duty_pct\"]}",
      "duty set" },
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "bad input" },
    { 503, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "no primary fan" },
    { 0 },
};

static const bb_route_t s_fan_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/fan",
    .tag       = "fan",
    .summary   = "Get fan readings",
    .responses = s_fan_get_responses,
    .handler   = NULL,  // set below at init
};

static const bb_route_t s_fan_post_route = {
    .method    = BB_HTTP_POST,
    .path      = "/api/fan",
    .tag       = "fan",
    .summary   = "Set fan duty cycle (raw duty; generic HAL control)",
    .responses = s_fan_post_responses,
    .handler   = NULL,  // set below at init
};

// ---------------------------------------------------------------------------
// GET handler
// ---------------------------------------------------------------------------

void bb_fan_routes_emit(bb_http_request_t *req)
{
    bb_fan_handle_t h = bb_fan_primary();
    bool present = (h != NULL);

    bb_fan_snapshot_t snap;
    bb_fan_snapshot(h, &snap);

    bb_json_t root = bb_json_obj_new();
    bb_json_obj_set_bool(root, "present", present);

    if (present && snap.rpm >= 0) {
        bb_json_obj_set_number(root, "rpm", (double)snap.rpm);
    } else {
        bb_json_obj_set_null(root, "rpm");
    }

    if (present && snap.duty_pct >= 0) {
        bb_json_obj_set_number(root, "duty_pct", (double)snap.duty_pct);
    } else {
        bb_json_obj_set_null(root, "duty_pct");
    }

    bb_http_route_run_extenders("fan", root);

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

static bb_err_t fan_get_handler(bb_http_request_t *req)
{
    bb_fan_routes_emit(req);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// POST handler — sets raw duty on the primary fan handle
// NOTE: TM's existing POST /api/fan sets autofan config; that reconciliation
// is deferred to P4. This BB route provides generic raw-duty control.
// ---------------------------------------------------------------------------

static bb_err_t fan_post_handler(bb_http_request_t *req)
{
    bb_fan_handle_t h = bb_fan_primary();
    if (!h) {
        bb_http_resp_set_status(req, 503);
        bb_json_t err = bb_json_obj_new();
        bb_json_obj_set_string(err, "error", "no primary fan");
        char *str = bb_json_serialize(err);
        bb_json_free(err);
        if (str) {
            bb_http_resp_set_type(req, "application/json");
            bb_http_resp_send_chunk(req, str, -1);
            bb_http_resp_send_chunk(req, NULL, 0);
            free(str);
        } else {
            bb_http_resp_send_chunk(req, NULL, 0);
        }
        return BB_ERR_INVALID_STATE;
    }

    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0 || body_len > 256) {
        bb_http_resp_set_status(req, 400);
        bb_json_t err = bb_json_obj_new();
        bb_json_obj_set_string(err, "error", "missing or oversized body");
        char *str = bb_json_serialize(err);
        bb_json_free(err);
        if (str) {
            bb_http_resp_set_type(req, "application/json");
            bb_http_resp_send_chunk(req, str, -1);
            bb_http_resp_send_chunk(req, NULL, 0);
            free(str);
        } else {
            bb_http_resp_send_chunk(req, NULL, 0);
        }
        return BB_ERR_INVALID_ARG;
    }

    char body[256];
    int n = bb_http_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        bb_http_resp_set_status(req, 400);
        bb_json_t err = bb_json_obj_new();
        bb_json_obj_set_string(err, "error", "read failed");
        char *str = bb_json_serialize(err);
        bb_json_free(err);
        if (str) {
            bb_http_resp_set_type(req, "application/json");
            bb_http_resp_send_chunk(req, str, -1);
            bb_http_resp_send_chunk(req, NULL, 0);
            free(str);
        } else {
            bb_http_resp_send_chunk(req, NULL, 0);
        }
        return BB_ERR_INVALID_ARG;
    }
    body[n] = '\0';

    bb_json_t parsed = bb_json_parse(body, (size_t)n);
    if (!parsed) {
        bb_http_resp_set_status(req, 400);
        bb_json_t err = bb_json_obj_new();
        bb_json_obj_set_string(err, "error", "invalid JSON");
        char *str = bb_json_serialize(err);
        bb_json_free(err);
        if (str) {
            bb_http_resp_set_type(req, "application/json");
            bb_http_resp_send_chunk(req, str, -1);
            bb_http_resp_send_chunk(req, NULL, 0);
            free(str);
        } else {
            bb_http_resp_send_chunk(req, NULL, 0);
        }
        return BB_ERR_INVALID_ARG;
    }

    // Extract duty_pct — must be present and a valid integer 0..100
    double duty_d = -1.0;
    int duty = -1;
    if (!bb_json_obj_get_number(parsed, "duty_pct", &duty_d)) {
        bb_json_free(parsed);
        bb_http_resp_set_status(req, 400);
        bb_json_t err = bb_json_obj_new();
        bb_json_obj_set_string(err, "error", "duty_pct required");
        char *str = bb_json_serialize(err);
        bb_json_free(err);
        if (str) {
            bb_http_resp_set_type(req, "application/json");
            bb_http_resp_send_chunk(req, str, -1);
            bb_http_resp_send_chunk(req, NULL, 0);
            free(str);
        } else {
            bb_http_resp_send_chunk(req, NULL, 0);
        }
        return BB_ERR_INVALID_ARG;
    }
    duty = (int)duty_d;
    bb_json_free(parsed);

    if (duty < 0 || duty > 100) {
        bb_http_resp_set_status(req, 400);
        bb_json_t err = bb_json_obj_new();
        bb_json_obj_set_string(err, "error", "duty_pct must be 0..100");
        char *str = bb_json_serialize(err);
        bb_json_free(err);
        if (str) {
            bb_http_resp_set_type(req, "application/json");
            bb_http_resp_send_chunk(req, str, -1);
            bb_http_resp_send_chunk(req, NULL, 0);
            free(str);
        } else {
            bb_http_resp_send_chunk(req, NULL, 0);
        }
        return BB_ERR_INVALID_ARG;
    }

    bb_fan_set_duty_pct(h, duty);

    bb_json_t resp = bb_json_obj_new();
    bb_json_obj_set_string(resp, "status", "ok");
    bb_json_obj_set_number(resp, "duty_pct", (double)duty);
    char *str = bb_json_serialize(resp);
    bb_json_free(resp);
    if (!str) {
        bb_http_resp_send_chunk(req, NULL, 0);
        return BB_ERR_NO_SPACE;
    }
    bb_http_resp_set_type(req, "application/json");
    bb_http_resp_send_chunk(req, str, -1);
    bb_http_resp_send_chunk(req, NULL, 0);
    free(str);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Init (regular-tier, order 1)
// ---------------------------------------------------------------------------

bb_err_t bb_fan_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    const char *schema = bb_http_route_assemble_schema(
        "fan", k_fan_schema_base, k_fan_schema_suffix);
    if (!schema) {
        bb_log_w(TAG, "fan schema assembly: malloc failed; schema will be NULL");
    }
    s_fan_get_responses[0].schema = schema;

    bb_route_t get_route = s_fan_get_route;
    get_route.handler = fan_get_handler;
    bb_err_t err = bb_http_register_described_route(server, &get_route);
    if (err != BB_OK) return err;

    bb_route_t post_route = s_fan_post_route;
    post_route.handler = fan_post_handler;
    err = bb_http_register_described_route(server, &post_route);
    if (err != BB_OK) return err;

    bb_log_i(TAG, "fan routes registered");
    return BB_OK;
}

#if CONFIG_BB_FAN_ROUTES_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_fan_routes, bb_fan_routes_init, 1);
#endif

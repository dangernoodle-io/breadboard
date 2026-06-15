// bb_telemetry — GET + PATCH /api/telemetry route.
//
// GET  builds {<name>: {fields}} per registered section.
// PATCH dispatches parsed body sub-objects to section patch_fn.
//       Returns 400 if a present section is read-only.
//       Returns 204 on success.
#include "bb_telemetry.h"
#include "bb_http.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bb_telemetry";

#define BB_TELEMETRY_BODY_MAX 8192

// ---------------------------------------------------------------------------
// Helper: send JSON error
// ---------------------------------------------------------------------------

static void send_json_error(bb_http_request_t *req, int status, const char *msg)
{
    bb_http_resp_set_status(req, status);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "error", msg);
    bb_http_resp_json_obj_end(&obj);
}

// ---------------------------------------------------------------------------
// GET /api/telemetry
// ---------------------------------------------------------------------------

static bb_err_t telemetry_get_handler(bb_http_request_t *req)
{
    bb_json_t root = bb_json_obj_new();
    if (!root) {
        send_json_error(req, 500, "out of memory");
        return BB_ERR_NO_SPACE;
    }

    bb_telemetry_build_get(root);
    bb_json_obj_set_bool(root, "pending_reboot", bb_telemetry_pending_reboot());

    char *json = bb_json_serialize(root);
    bb_json_free(root);
    if (!json) {
        send_json_error(req, 500, "serialize failed");
        return BB_ERR_NO_SPACE;
    }

    bb_http_resp_set_type(req, "application/json");
    bb_http_resp_send_chunk(req, json, (int)strlen(json));
    bb_http_resp_send_chunk(req, NULL, 0);
    bb_json_free_str(json);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// PATCH /api/telemetry
// ---------------------------------------------------------------------------

static bb_err_t telemetry_patch_handler(bb_http_request_t *req)
{
    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0 || body_len > BB_TELEMETRY_BODY_MAX) {
        send_json_error(req, 400, "missing or oversized body");
        return BB_ERR_INVALID_ARG;
    }

    char *body = malloc((size_t)body_len + 1);
    if (!body) {
        send_json_error(req, 400, "out of memory");
        return BB_ERR_NO_SPACE;
    }

    int n = bb_http_req_recv(req, body, (size_t)(body_len + 1));
    if (n < 0) {
        free(body);
        send_json_error(req, 400, "read failed");
        return BB_ERR_INVALID_ARG;
    }
    body[n] = '\0';

    bb_json_t parsed = bb_json_parse(body, (size_t)n);
    free(body);
    if (!parsed) {
        send_json_error(req, 400, "invalid JSON");
        return BB_ERR_INVALID_ARG;
    }

    bb_err_t rc = bb_telemetry_dispatch_patch(parsed);
    bb_json_free(parsed);

    if (rc == BB_ERR_INVALID_ARG) {
        send_json_error(req, 400, "PATCH on read-only section");
        return rc;
    }
    if (rc == BB_ERR_CONFLICT) {
        send_json_error(req, 409,
                        "another telemetry sink is active; disable it first");
        return rc;
    }
    if (rc != BB_OK) {
        send_json_error(req, 500, "patch failed");
        return rc;
    }

    // B1-289: config persisted to NVS; reboot required to apply.
    bb_http_resp_set_status(req, 200);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_bool(&obj, "reboot_required", true);
    bb_http_resp_json_obj_end(&obj);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Route descriptors — GET responses[0].schema filled at init from registered sections
// ---------------------------------------------------------------------------

static bb_route_response_t s_telemetry_get_responses[] = {
    { 200, "application/json",
      NULL,  // filled by bb_telemetry_assemble_get_schema() at init
      "Telemetry sections (mqtt, http, publisher)" },
    { 0 },
};

static const bb_route_response_t s_telemetry_patch_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"reboot_required\":{\"type\":\"boolean\"}},"
      "\"required\":[\"reboot_required\"]}",
      "settings persisted to NVS; reboot required to apply" },
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "bad or missing request body, or read-only section" },
    { 409, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "another telemetry sink is already active" },
    { 0 },
};

static bb_route_t s_telemetry_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/telemetry",
    .tag       = "telemetry",
    .summary   = "Get telemetry configuration (mqtt, http, publisher sections)",
    .responses = s_telemetry_get_responses,
    .handler   = telemetry_get_handler,
};

static const bb_route_t s_telemetry_patch_route = {
    .method               = BB_HTTP_PATCH,
    .path                 = "/api/telemetry",
    .tag                  = "telemetry",
    .summary              = "Patch telemetry configuration sections",
    .request_content_type = "application/json",
    .request_schema       =
        "{\"type\":\"object\","
        "\"description\":\"Keys are section names (mqtt, http, publisher); "
                         "values are section-specific patch objects\"}",
    .responses = s_telemetry_patch_responses,
    .handler   = telemetry_patch_handler,
};

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bb_err_t bb_telemetry_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    // Build real composed GET schema from registered section schema_props.
    // Sections must be registered (PRE_HTTP tier) before this init (order 5).
    char *schema = bb_telemetry_assemble_get_schema();
    if (!schema) {
        bb_log_w(TAG, "schema assembly: malloc failed; GET schema will be NULL");
    }
    s_telemetry_get_responses[0].schema = schema;

    // Freeze: reject late registrations (all sections must be PRE_HTTP).
    bb_telemetry_freeze();

    bb_err_t rc = bb_http_register_described_route(server, &s_telemetry_get_route);
    if (rc != BB_OK) return rc;
    rc = bb_http_register_described_route(server, &s_telemetry_patch_route);
    if (rc != BB_OK) return rc;

    bb_log_i(TAG, "telemetry routes registered");
    return BB_OK;
}

#if CONFIG_BB_TELEMETRY_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_telemetry, bb_telemetry_init, 5);
#endif

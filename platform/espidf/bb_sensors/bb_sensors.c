// bb_sensors — sectioned /api/sensors endpoint (fan/power/thermal).
//
// Compiled on both ESP-IDF and host (via platform/host/bb_sensors/ shim).
// The route-registration path uses bb_http which is ESP-IDF + host-stub.
#include "bb_sensors.h"
#include "bb_section.h"
#include "bb_fan_routes.h"
#include "bb_fan.h"
#include "bb_power_routes.h"
#include "bb_thermal.h"
#include "bb_http.h"
#include "bb_http_body.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../../../components/bb_sensors/bb_sensors_schema_priv.h"

static const char *TAG = "bb_sensors";

// File-scope section registry for /api/sensors.
static bb_section_registry_t s_sensors_reg = { .tag = "bb_sensors" };

// ---------------------------------------------------------------------------
// Fan section: GET get_fn + PATCH patch_fn
// ---------------------------------------------------------------------------

static void fan_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;
    bb_fan_emit_section(section);
}

// Fan PATCH: apply {duty_pct} or autofan config fields.
// Mirrors the old POST /api/fan logic (now section patch_fn).
static bb_err_t fan_section_patch(bb_json_t patch_body, void *ctx)
{
    (void)ctx;
    bb_fan_handle_t h = bb_fan_primary();
    if (!h) return BB_ERR_INVALID_STATE;

#ifdef CONFIG_BB_FAN_AUTOFAN
    // Autofan PATCH: optional fields, partial update.
    // Validate all supplied fields BEFORE applying any — atomicity guarantee.
    bb_fan_autofan_cfg_t cfg;
    bb_fan_get_autofan_cfg(h, &cfg);

    double d;
    bool b;
    if (bb_json_obj_get_number(patch_body, "manual_pct", &d)) {
        if (d < 0.0 || d > 100.0) return BB_ERR_INVALID_ARG;
        cfg.manual_pct = (int)d;
    }
    if (bb_json_obj_get_number(patch_body, "min_pct", &d)) {
        if (d < 0.0 || d > 100.0) return BB_ERR_INVALID_ARG;
        cfg.min_pct = (int)d;
    }
    if (bb_json_obj_get_number(patch_body, "die_target_c", &d)) {
        if (d <= 0.0) return BB_ERR_INVALID_ARG;
        cfg.die_target_c = (float)d;
    }
    if (bb_json_obj_get_number(patch_body, "vr_target_c", &d)) {
        if (d <= 0.0) return BB_ERR_INVALID_ARG;
        cfg.aux_target_c = (float)d;
    }
    if (bb_json_obj_get_bool(patch_body, "autofan", &b)) cfg.enabled = b;

    bb_fan_set_autofan(h, &cfg);
#else
    // Raw duty PATCH: requires duty_pct field.
    double duty_d = -1.0;
    if (!bb_json_obj_get_number(patch_body, "duty_pct", &duty_d)) {
        return BB_ERR_INVALID_ARG;
    }
    int duty = (int)duty_d;
    if (duty < 0 || duty > 100) return BB_ERR_INVALID_ARG;
    bb_fan_set_duty_pct(h, duty);
#endif /* CONFIG_BB_FAN_AUTOFAN */

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Power section: GET only (read-only section).
// ---------------------------------------------------------------------------

static void power_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;
    bb_power_emit_section(section);
}

// ---------------------------------------------------------------------------
// Thermal section: GET only (read-only section).
// ---------------------------------------------------------------------------

static void thermal_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;
    bb_thermal_emit_section(section);
}

// ---------------------------------------------------------------------------
// Public registration API
// ---------------------------------------------------------------------------

bb_err_t bb_sensors_register_section(const char *name,
                                      bb_section_get_fn get,
                                      bb_section_patch_fn patch,
                                      void *ctx,
                                      const char *schema_props)
{
    return bb_section_register(&s_sensors_reg, name, get, patch, ctx, schema_props);
}

// ---------------------------------------------------------------------------
// JSON send helper
// ---------------------------------------------------------------------------

static bb_err_t send_json_tree(bb_http_request_t *req, bb_json_t root)
{
    char *str = bb_json_serialize(root);
    if (!str) return BB_ERR_NO_SPACE;
    bb_err_t err = bb_http_resp_set_type(req, "application/json");
    if (err == BB_OK) err = bb_http_resp_send_chunk(req, str, -1);
    if (err == BB_OK) err = bb_http_resp_send_chunk(req, NULL, 0);
    free(str);
    return err;
}

static void send_json_error(bb_http_request_t *req, int status, const char *msg)
{
    bb_http_resp_set_status(req, status);
    bb_json_t err_obj = bb_json_obj_new();
    bb_json_obj_set_string(err_obj, "error", msg);
    char *str = bb_json_serialize(err_obj);
    bb_json_free(err_obj);
    if (str) {
        bb_http_resp_set_type(req, "application/json");
        bb_http_resp_send_chunk(req, str, -1);
        bb_http_resp_send_chunk(req, NULL, 0);
        free(str);
    } else {
        bb_http_resp_send_chunk(req, NULL, 0);
    }
}

// ---------------------------------------------------------------------------
// GET /api/sensors handler
// ---------------------------------------------------------------------------

static bb_err_t sensors_get_handler(bb_http_request_t *req)
{
    bb_json_t root = bb_json_obj_new();
    bb_section_build_get(&s_sensors_reg, root);
    bb_err_t err = send_json_tree(req, root);
    bb_json_free(root);
    return err;
}

// ---------------------------------------------------------------------------
// PATCH /api/sensors handler
// ---------------------------------------------------------------------------

static bb_err_t sensors_patch_handler(bb_http_request_t *req)
{
    // Pre-check body size to preserve the original 400 vs 500 error-status
    // distinction: oversized → 400, OOM → 500.
    {
        int bl = bb_http_req_body_len(req);
        if (bl <= 0 || bl > CONFIG_BB_SENSORS_PATCH_BODY_MAX) {
            send_json_error(req, 400, "missing or oversized body");
            return BB_ERR_INVALID_ARG;
        }
    }
    char *body = NULL;
    int   n    = 0;
    bb_err_t brc = bb_http_req_recv_body_alloc(req, CONFIG_BB_SENSORS_PATCH_BODY_MAX, &body, &n);
    if (brc == BB_ERR_NO_SPACE) {
        // Only reachable here if OOM (size check passed above).
        send_json_error(req, 500, "out of memory");
        return brc;
    }
    if (brc != BB_OK) {
        send_json_error(req, 400, "read failed");
        return brc;
    }

    bb_json_t parsed = bb_json_parse(body, (size_t)n);
    free(body);
    if (!parsed) {
        send_json_error(req, 400, "invalid JSON");
        return BB_ERR_INVALID_ARG;
    }

    bb_err_t rc = bb_section_dispatch_patch(&s_sensors_reg, parsed);
    bb_json_free(parsed);

    if (rc == BB_ERR_INVALID_ARG) {
        send_json_error(req, 400, "PATCH on read-only section or missing required field");
        return rc;
    }
    if (rc == BB_ERR_INVALID_STATE) {
        send_json_error(req, 503, "no primary fan");
        return rc;
    }
    if (rc != BB_OK) {
        send_json_error(req, 400, "patch failed");
        return rc;
    }

    bb_http_resp_no_content(req);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Route descriptors
// ---------------------------------------------------------------------------

static bb_route_response_t s_sensors_get_responses[] = {
    { 200, "application/json",
      NULL,  // filled by bb_section_assemble_schema() at init
      "sensor readings (fan/power/thermal sections)" },
    { 0 },
};

static const bb_route_response_t s_sensors_patch_responses[] = {
    { 204, NULL, NULL, "patch applied" },
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "bad input or read-only section" },
    { 503, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "no primary fan" },
    { 0 },
};

static const bb_route_t s_sensors_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/sensors",
    .tag       = "sensors",
    .summary   = "Get sensor readings (fan/power/thermal sections)",
    .responses = s_sensors_get_responses,
    .handler   = sensors_get_handler,
};

static const bb_route_t s_sensors_patch_route = {
    .method               = BB_HTTP_PATCH,
    .path                 = "/api/sensors",
    .tag                  = "sensors",
    .summary              = "Patch writable sensor sections (fan: duty_pct or autofan config)",
    .request_content_type = "application/json",
    .request_schema       = k_sensors_patch_request_schema,
    .responses            = s_sensors_patch_responses,
    .handler              = sensors_patch_handler,
};

// ---------------------------------------------------------------------------
// Init (regular-tier, order 1)
// ---------------------------------------------------------------------------

bb_err_t bb_sensors_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    // Register built-in sections (fan is PATCH-capable; power/thermal read-only).
    bb_err_t err;
    err = bb_section_register(&s_sensors_reg, "fan",
                              fan_section_get, fan_section_patch, NULL,
                              k_sensors_fan_schema);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to register fan section: %d", (int)err);
        return err;
    }

    err = bb_section_register(&s_sensors_reg, "power",
                              power_section_get, NULL, NULL,
                              k_sensors_power_schema);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to register power section: %d", (int)err);
        return err;
    }

    err = bb_section_register(&s_sensors_reg, "thermal",
                              thermal_section_get, NULL, NULL,
                              k_sensors_thermal_schema);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to register thermal section: %d", (int)err);
        return err;
    }

    // Freeze: no more section registrations after this point.
    s_sensors_get_responses[0].schema = bb_section_freeze_and_assemble(&s_sensors_reg, k_sensors_base, k_sensors_suffix);

    // Register GET /api/sensors.
    static bb_route_t get_route;
    get_route = s_sensors_get_route;
    err = bb_http_register_described_route(server, &get_route);
    if (err != BB_OK) return err;

    // Register PATCH /api/sensors.
    static bb_route_t patch_route;
    patch_route = s_sensors_patch_route;
    err = bb_http_register_described_route(server, &patch_route);
    if (err != BB_OK) return err;

    bb_log_i(TAG, "sensors routes registered (/api/sensors GET+PATCH)");
    return BB_OK;
}

#if CONFIG_BB_SENSORS_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_sensors, bb_sensors_init, 1);
#endif



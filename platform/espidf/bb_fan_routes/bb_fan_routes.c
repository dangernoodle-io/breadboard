#include "bb_fan_routes.h"
#include "bb_fan.h"
#include "bb_http.h"
#include "bb_http_extender.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "bb_fan_routes";

#ifdef CONFIG_BB_FAN_AUTOFAN
// Persist callback registered by the consumer (e.g. TM) to save config to NVS.
// Fired only from the POST handler path; never from bb_fan_set_autofan() directly.
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
// Base JSON-Schema for GET /api/fan 200 response
// ---------------------------------------------------------------------------

#ifdef CONFIG_BB_FAN_AUTOFAN
static const char k_fan_schema_base[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"rpm\":{\"type\":[\"integer\",\"null\"]},"
    "\"duty_pct\":{\"type\":[\"integer\",\"null\"]},"
    "\"autofan\":{\"type\":\"boolean\",\"description\":\"autofan enabled\"},"
    "\"die_target_c\":{\"type\":\"number\",\"description\":\"ASIC die target temperature\"},"
    "\"vr_target_c\":{\"type\":\"number\",\"description\":\"VR target temperature\"},"
    "\"manual_pct\":{\"type\":\"integer\",\"description\":\"manual duty % when autofan disabled\"},"
    "\"min_pct\":{\"type\":\"integer\",\"description\":\"minimum fan duty %\"},"
    "\"die_ema_c\":{\"type\":[\"number\",\"null\"],\"description\":\"filtered ASIC die temperature\"},"
    "\"vr_ema_c\":{\"type\":[\"number\",\"null\"],\"description\":\"filtered VR temperature\"},"
    "\"pid_input_c\":{\"type\":[\"number\",\"null\"],\"description\":\"PID input selected by max(err/target) ratio\"},"
    "\"pid_input_src\":{\"type\":\"string\",\"enum\":[\"die\",\"vr\"],\"description\":\"which sensor is driving PID: die or vr\"}";
#else
static const char k_fan_schema_base[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"rpm\":{\"type\":[\"integer\",\"null\"]},"
    "\"duty_pct\":{\"type\":[\"integer\",\"null\"]}";
#endif

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

#ifdef CONFIG_BB_FAN_AUTOFAN
static const bb_route_response_t s_fan_post_responses[] = {
    { 204, NULL, NULL, "config applied" },
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
#else
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
#endif

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
#ifdef CONFIG_BB_FAN_AUTOFAN
    .summary   = "Set autofan config (autofan, die_target_c, vr_target_c, manual_pct, min_pct)",
#else
    .summary   = "Set fan duty cycle (raw duty; generic HAL control)",
#endif
    .responses = s_fan_post_responses,
    .handler   = NULL,  // set below at init
};

// ---------------------------------------------------------------------------
// Helper: send JSON error
// ---------------------------------------------------------------------------

static void send_json_error(bb_http_request_t *req, int status, const char *msg)
{
    bb_http_resp_set_status(req, status);
    bb_json_t err = bb_json_obj_new();
    bb_json_obj_set_string(err, "error", msg);
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
}

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

#ifdef CONFIG_BB_FAN_AUTOFAN
    if (present && h) {
        // Autofan config fields (consumer contract: autofan, die_target_c,
        // vr_target_c, manual_pct, min_pct). cfg.aux_target_c → "vr_target_c".
        bb_fan_autofan_cfg_t cfg;
        bb_fan_get_autofan_cfg(h, &cfg);
        bb_json_obj_set_bool(root,   "autofan",      cfg.enabled);
        bb_json_obj_set_number(root, "die_target_c", (double)cfg.die_target_c);
        bb_json_obj_set_number(root, "vr_target_c",  (double)cfg.aux_target_c);
        bb_json_obj_set_number(root, "manual_pct",   (double)cfg.manual_pct);
        bb_json_obj_set_number(root, "min_pct",      (double)cfg.min_pct);

        // Autofan telemetry fields.
        bb_fan_autofan_telemetry_t tel;
        bb_fan_get_autofan_telemetry(h, &tel);

        if (tel.die_ema_c >= 0.0f) {
            bb_json_obj_set_number(root, "die_ema_c", (double)tel.die_ema_c);
        } else {
            bb_json_obj_set_null(root, "die_ema_c");
        }
        if (tel.aux_ema_c >= 0.0f) {
            bb_json_obj_set_number(root, "vr_ema_c", (double)tel.aux_ema_c);
        } else {
            bb_json_obj_set_null(root, "vr_ema_c");
        }
        if (tel.pid_input_c >= 0.0f) {
            bb_json_obj_set_number(root, "pid_input_c", (double)tel.pid_input_c);
        } else {
            bb_json_obj_set_null(root, "pid_input_c");
        }
        // Wire-layer mapping: internal "aux" → TM contract name "vr".
        const char *src = tel.pid_input_src ? tel.pid_input_src : "";
        if (src[0] == 'a') src = "vr";  // "aux" → "vr"
        bb_json_obj_set_string(root, "pid_input_src", src);
    }
#endif /* CONFIG_BB_FAN_AUTOFAN */

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
// POST handler
// ---------------------------------------------------------------------------

#ifdef CONFIG_BB_FAN_AUTOFAN

static bb_err_t fan_post_handler(bb_http_request_t *req)
{
    bb_fan_handle_t h = bb_fan_primary();
    if (!h) {
        send_json_error(req, 503, "no primary fan");
        return BB_ERR_INVALID_STATE;
    }

    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0 || body_len > 256) {
        send_json_error(req, 400, "missing or oversized body");
        return BB_ERR_INVALID_ARG;
    }

    char body[256];
    int n = bb_http_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        send_json_error(req, 400, "read failed");
        return BB_ERR_INVALID_ARG;
    }
    body[n] = '\0';

    bb_json_t parsed = bb_json_parse(body, (size_t)n);
    if (!parsed) {
        send_json_error(req, 400, "invalid JSON");
        return BB_ERR_INVALID_ARG;
    }

    // Get current config as baseline — partial updates allowed
    bb_fan_autofan_telemetry_t tel;
    bb_fan_get_autofan_telemetry(h, &tel);

    // We need the current cfg; since we don't have a cfg getter, use a
    // snapshot-based approach: read from parsed fields over a default.
    // Build the cfg from fields present in the JSON body.
    // For fields absent from the body, we can't easily read current cfg without
    // a getter — add bb_fan_get_autofan to the public API.
    // For now, parse all fields (absent fields keep their current value by
    // re-applying set_autofan after reading). We need a cfg getter.
    // Add it to the implementation separately.
    // Simpler: parse only what's present; call individual setters (but we
    // only have set_autofan which takes the whole struct).
    // Solution: add bb_fan_get_autofan_cfg() to bb_fan.c and header.
    // For the initial implementation, read cfg via a bb_fan_get_autofan_cfg call.
    bb_fan_autofan_cfg_t cfg;
    bb_fan_get_autofan_cfg(h, &cfg);

    // Parse optional fields; only update what's present
    double d;
    bool b;

    if (bb_json_obj_get_bool(parsed, "autofan", &b)) {
        cfg.enabled = b;
    }
    if (bb_json_obj_get_number(parsed, "die_target_c", &d)) {
        cfg.die_target_c = (float)d;
    }
    // Accept vr_target_c (TM field name) as alias for aux_target_c
    if (bb_json_obj_get_number(parsed, "vr_target_c", &d)) {
        cfg.aux_target_c = (float)d;
    }
    if (bb_json_obj_get_number(parsed, "manual_pct", &d)) {
        cfg.manual_pct = (int)d;
    }
    if (bb_json_obj_get_number(parsed, "min_pct", &d)) {
        cfg.min_pct = (int)d;
    }

    bb_json_free(parsed);

    bb_fan_set_autofan(h, &cfg);

    // Invoke persist callback if registered (POST path only — not from direct set_autofan).
    if (s_persist_cb) {
        s_persist_cb(s_persist_ctx, &cfg);
    }

    bb_http_resp_no_content(req);
    return BB_OK;
}

#else  /* !CONFIG_BB_FAN_AUTOFAN */

static bb_err_t fan_post_handler(bb_http_request_t *req)
{
    bb_fan_handle_t h = bb_fan_primary();
    if (!h) {
        send_json_error(req, 503, "no primary fan");
        return BB_ERR_INVALID_STATE;
    }

    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0 || body_len > 256) {
        send_json_error(req, 400, "missing or oversized body");
        return BB_ERR_INVALID_ARG;
    }

    char body[256];
    int n = bb_http_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        send_json_error(req, 400, "read failed");
        return BB_ERR_INVALID_ARG;
    }
    body[n] = '\0';

    bb_json_t parsed = bb_json_parse(body, (size_t)n);
    if (!parsed) {
        send_json_error(req, 400, "invalid JSON");
        return BB_ERR_INVALID_ARG;
    }

    double duty_d = -1.0;
    int duty = -1;
    if (!bb_json_obj_get_number(parsed, "duty_pct", &duty_d)) {
        bb_json_free(parsed);
        send_json_error(req, 400, "duty_pct required");
        return BB_ERR_INVALID_ARG;
    }
    duty = (int)duty_d;
    bb_json_free(parsed);

    if (duty < 0 || duty > 100) {
        send_json_error(req, 400, "duty_pct must be 0..100");
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

#endif /* CONFIG_BB_FAN_AUTOFAN */

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

    // static: registry stores this pointer; descriptor must outlive init (B 405-walk deref)
    static bb_route_t get_route;
    get_route = s_fan_get_route;
    get_route.handler = fan_get_handler;
    bb_err_t err = bb_http_register_described_route(server, &get_route);
    if (err != BB_OK) return err;

    // static: registry stores this pointer; descriptor must outlive init (B 405-walk deref)
    static bb_route_t post_route;
    post_route = s_fan_post_route;
    post_route.handler = fan_post_handler;
    err = bb_http_register_described_route(server, &post_route);
    if (err != BB_OK) return err;

    bb_log_i(TAG, "fan routes registered");
    return BB_OK;
}

#if CONFIG_BB_FAN_ROUTES_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_fan_routes, bb_fan_routes_init, 1);
#endif

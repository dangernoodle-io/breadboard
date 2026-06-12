// bb_mqtt_routes host twin — provides test hooks and a minimal route
// implementation backed by the in-memory NVS store.
//
// The actual GET/PATCH route logic is compiled from the espidf source on
// the host (same pattern as bb_fan_routes, bb_power_routes). This file only
// provides the test-hook surface.
#include "bb_mqtt_routes.h"
#include "bb_mqtt.h"
#include "bb_nv.h"
#include "bb_http.h"
#include "bb_json.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define BB_MQTT_NVS_NS  "bb_mqtt"
#define BB_MQTT_STR_MAX 64
#define BB_MQTT_BODY_MAX 4096

// Shared client ref (mirrors espidf side).
static bb_mqtt_t *s_client_ref = NULL;

void bb_mqtt_routes_set_client(bb_mqtt_t *ref)
{
    s_client_ref = ref;
}

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
// GET handler
// ---------------------------------------------------------------------------

static bb_err_t mqtt_get_handler(bb_http_request_t *req)
{
    char uri[BB_MQTT_STR_MAX]       = {0};
    char client_id[BB_MQTT_STR_MAX] = {0};
    char username[BB_MQTT_STR_MAX]  = {0};
    char password[BB_MQTT_STR_MAX]  = {0};
    char enabled_str[4]             = "0";
    char tls_str[4]                 = "0";

    bb_nv_get_str(BB_MQTT_NVS_NS, "uri",       uri,         sizeof(uri),         "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "client_id", client_id,   sizeof(client_id),   "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "username",  username,    sizeof(username),    "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "password",  password,    sizeof(password),    "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "enabled",   enabled_str, sizeof(enabled_str), "0");
    bb_nv_get_str(BB_MQTT_NVS_NS, "tls",       tls_str,     sizeof(tls_str),     "0");

    char ca_probe[8]   = {0};
    char cert_probe[8] = {0};
    char key_probe[8]  = {0};
    bb_nv_get_str(BB_MQTT_NVS_NS, "tls_ca",   ca_probe,   sizeof(ca_probe),   "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "tls_cert", cert_probe, sizeof(cert_probe), "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "tls_key",  key_probe,  sizeof(key_probe),  "");

    bool ca_set   = (ca_probe[0]   != '\0');
    bool cert_set = (cert_probe[0] != '\0');
    bool key_set  = (key_probe[0]  != '\0');
    bool tls_on   = (tls_str[0] == '1');
    bool enabled  = (enabled_str[0] == '1');

    bool connected = false;
    if (s_client_ref && *s_client_ref) {
        connected = bb_mqtt_is_connected(*s_client_ref);
    } else {
        bb_mqtt_t def = bb_mqtt_default();
        if (def) connected = bb_mqtt_is_connected(def);
    }

    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;
    bb_http_resp_json_obj_set_str(&obj, "uri",       uri);
    bb_http_resp_json_obj_set_str(&obj, "client_id", client_id);
    bb_http_resp_json_obj_set_str(&obj, "username",  username);
    bb_http_resp_json_obj_set_str(&obj, "password",  password[0] ? "***" : "");
    bb_http_resp_json_obj_set_bool(&obj, "tls",       tls_on);
    bb_http_resp_json_obj_set_bool(&obj, "ca_set",    ca_set);
    bb_http_resp_json_obj_set_bool(&obj, "cert_set",  cert_set);
    bb_http_resp_json_obj_set_bool(&obj, "key_set",   key_set);
    bb_http_resp_json_obj_set_bool(&obj, "enabled",   enabled);
    bb_http_resp_json_obj_set_bool(&obj, "connected", connected);
    return bb_http_resp_json_obj_end(&obj);
}

// ---------------------------------------------------------------------------
// PATCH handler
// ---------------------------------------------------------------------------

static bb_err_t mqtt_patch_handler(bb_http_request_t *req)
{
    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0 || body_len > BB_MQTT_BODY_MAX) {
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

    char tmp[BB_MQTT_BODY_MAX + 1];

    if (bb_json_obj_get_string(parsed, "uri",       tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "uri", tmp);
    }
    if (bb_json_obj_get_string(parsed, "client_id", tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "client_id", tmp);
    }
    if (bb_json_obj_get_string(parsed, "username",  tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "username", tmp);
    }
    if (bb_json_obj_get_string(parsed, "password",  tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "password", tmp);
    }
    if (bb_json_obj_get_string(parsed, "tls_ca",   tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "tls_ca", tmp);
    }
    if (bb_json_obj_get_string(parsed, "tls_cert", tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "tls_cert", tmp);
    }
    if (bb_json_obj_get_string(parsed, "tls_key",  tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "tls_key", tmp);
    }

    bool b;
    if (bb_json_obj_get_bool(parsed, "tls", &b)) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "tls", b ? "1" : "0");
    }
    if (bb_json_obj_get_bool(parsed, "enabled", &b)) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "enabled", b ? "1" : "0");
    }

    bb_json_free(parsed);

    bb_http_resp_no_content(req);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Route descriptors
// ---------------------------------------------------------------------------

static const bb_route_response_t s_mqtt_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"uri\":{\"type\":\"string\"},"
      "\"client_id\":{\"type\":\"string\"},"
      "\"username\":{\"type\":\"string\"},"
      "\"password\":{\"type\":\"string\"},"
      "\"tls\":{\"type\":\"boolean\"},"
      "\"ca_set\":{\"type\":\"boolean\"},"
      "\"cert_set\":{\"type\":\"boolean\"},"
      "\"key_set\":{\"type\":\"boolean\"},"
      "\"enabled\":{\"type\":\"boolean\"},"
      "\"connected\":{\"type\":\"boolean\"}},"
      "\"required\":[\"uri\",\"tls\",\"ca_set\",\"cert_set\","
                   "\"key_set\",\"enabled\",\"connected\"]}",
      "MQTT configuration and connection status" },
    { 0 },
};

static const bb_route_response_t s_mqtt_patch_responses[] = {
    { 204, NULL, NULL, "settings persisted" },
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "bad or missing request body" },
    { 0 },
};

static const bb_route_t s_mqtt_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/mqtt",
    .tag       = "mqtt",
    .summary   = "Get MQTT configuration and connection state",
    .responses = s_mqtt_get_responses,
    .handler   = mqtt_get_handler,
};

static const bb_route_t s_mqtt_patch_route = {
    .method               = BB_HTTP_PATCH,
    .path                 = "/api/mqtt",
    .tag                  = "mqtt",
    .summary              = "Persist MQTT configuration fields to NVS",
    .request_content_type = "application/json",
    .request_schema       =
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"uri\":{\"type\":\"string\"},"
        "\"client_id\":{\"type\":\"string\"},"
        "\"username\":{\"type\":\"string\"},"
        "\"password\":{\"type\":\"string\"},"
        "\"tls_ca\":{\"type\":\"string\"},"
        "\"tls_cert\":{\"type\":\"string\"},"
        "\"tls_key\":{\"type\":\"string\"},"
        "\"tls\":{\"type\":\"boolean\"},"
        "\"enabled\":{\"type\":\"boolean\"}}}",
    .responses = s_mqtt_patch_responses,
    .handler   = mqtt_patch_handler,
};

bb_err_t bb_mqtt_routes_init(bb_http_handle_t server)
{
    (void)server;
    // Host: route dispatch goes through test_http_utils; just register.
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_MQTT_ROUTES_TESTING

void bb_mqtt_routes_reset_for_test(void)
{
    s_client_ref = NULL;
    bb_nv_host_str_store_reset();
}

// Expose GET and PATCH handlers for test_http_utils invocation.
bb_err_t bb_mqtt_routes_get_handler_for_test(bb_http_request_t *req)
{
    return mqtt_get_handler(req);
}

bb_err_t bb_mqtt_routes_patch_handler_for_test(bb_http_request_t *req)
{
    return mqtt_patch_handler(req);
}

#endif /* BB_MQTT_ROUTES_TESTING */

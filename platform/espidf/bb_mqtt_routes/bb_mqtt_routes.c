// bb_mqtt_routes — GET + PATCH /api/mqtt.
//
// GET  reads NVS "bb_mqtt" and reports connection state; masks secrets.
// PATCH persists fields to NVS "bb_mqtt" (including TLS PEM material).
// Returns 204 on success; 400 on bad/missing body or invalid JSON.
#include "bb_mqtt_routes.h"
#include "bb_mqtt.h"
#include "bb_http.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_nv.h"
#include "bb_registry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bb_mqtt_routes";

#define BB_MQTT_NVS_NS      "bb_mqtt"
#define BB_MQTT_BODY_MAX    4096   // allow long PEM blocks in PATCH body
#define BB_MQTT_URI_MAX     128
#define BB_MQTT_STR_MAX     64

// Module-level handle: bb_mqtt_routes does not own an MQTT connection.
// The connected state is queried from the module-level auto-handle via a
// weak accessor. On ESP-IDF: the EARLY-registered auto-client is referenced
// through the public bb_mqtt_is_connected API if we expose the handle.
// For simplicity, we track connection state via a settable pointer.
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
// GET /api/mqtt
// ---------------------------------------------------------------------------

static bb_err_t mqtt_get_handler(bb_http_request_t *req)
{
    char uri[BB_MQTT_URI_MAX]  = {0};
    char client_id[BB_MQTT_STR_MAX] = {0};
    char username[BB_MQTT_STR_MAX]  = {0};
    char password[BB_MQTT_STR_MAX]  = {0};
    char tls_ca[4]             = {0};
    char tls_cert[4]           = {0};
    char tls_key_buf[4]        = {0};
    char enabled_str[4]        = "0";
    char tls_str[4]            = "0";

    bb_nv_get_str(BB_MQTT_NVS_NS, "uri",       uri,       sizeof(uri),       "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "client_id", client_id, sizeof(client_id), "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "username",  username,  sizeof(username),  "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "password",  password,  sizeof(password),  "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "enabled",   enabled_str, sizeof(enabled_str), "0");
    bb_nv_get_str(BB_MQTT_NVS_NS, "tls",       tls_str,   sizeof(tls_str),   "0");

    // Check whether TLS PEM fields are set (without exposing content).
    // We probe with a 1-byte buffer; if bb_nv_get_str fills it, the key exists.
    // Actual detection: try to read a small sentinel — if value is non-empty, it's set.
    // Use a larger buffer to capture first byte only.
    char ca_probe[8] = {0}, cert_probe[8] = {0}, key_probe[8] = {0};
    bb_nv_get_str(BB_MQTT_NVS_NS, "tls_ca",   ca_probe,   sizeof(ca_probe),   "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "tls_cert", cert_probe, sizeof(cert_probe), "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "tls_key",  key_probe,  sizeof(key_probe),  "");

    (void)tls_ca; (void)tls_cert; (void)tls_key_buf;  // unused large buffers above

    bool ca_set   = (ca_probe[0]   != '\0');
    bool cert_set = (cert_probe[0] != '\0');
    bool key_set  = (key_probe[0]  != '\0');
    bool tls_on   = (tls_str[0] == '1');
    bool enabled  = (enabled_str[0] == '1');

    // Connection state: prefer the explicitly-set client ref; fall back to
    // the EARLY-registered auto-handle via bb_mqtt_default() so that the
    // auto-connect path (which never calls bb_mqtt_routes_set_client) still
    // reports the real connected state.
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
    // Mask password: report presence as bool string or "***" sentinel.
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
// PATCH /api/mqtt
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

    // Persist each present field to NVS.
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

    // TLS PEM fields — stored in "bb_mqtt" NVS NS where bb_tls_creds_resolve will find them.
    if (bb_json_obj_get_string(parsed, "tls_ca",   tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "tls_ca", tmp);
    }
    if (bb_json_obj_get_string(parsed, "tls_cert", tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "tls_cert", tmp);
    }
    if (bb_json_obj_get_string(parsed, "tls_key",  tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "tls_key", tmp);
    }

    // Boolean fields stored as "0"/"1" string.
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
      "\"password\":{\"type\":\"string\",\"description\":\"masked: empty or ***\"},"
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
        "\"tls_ca\":{\"type\":\"string\",\"description\":\"PEM CA certificate\"},"
        "\"tls_cert\":{\"type\":\"string\",\"description\":\"PEM client certificate\"},"
        "\"tls_key\":{\"type\":\"string\",\"description\":\"PEM client private key\"},"
        "\"tls\":{\"type\":\"boolean\"},"
        "\"enabled\":{\"type\":\"boolean\"}}}",
    .responses = s_mqtt_patch_responses,
    .handler   = mqtt_patch_handler,
};

// ---------------------------------------------------------------------------
// Init (regular-tier)
// ---------------------------------------------------------------------------

bb_err_t bb_mqtt_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t rc = bb_http_register_described_route(server, &s_mqtt_get_route);
    if (rc != BB_OK) return rc;
    rc = bb_http_register_described_route(server, &s_mqtt_patch_route);
    if (rc != BB_OK) return rc;

    bb_log_i(TAG, "mqtt routes registered");
    return BB_OK;
}

#if CONFIG_BB_MQTT_ROUTES_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_mqtt_routes, bb_mqtt_routes_init, 5);
#endif

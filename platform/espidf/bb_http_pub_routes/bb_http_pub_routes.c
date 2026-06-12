// bb_http_pub_routes — GET + PATCH /api/httppub.
//
// GET  reads NVS "bb_http_pub" and reports configuration; masks TLS secrets.
// PATCH persists fields to NVS "bb_http_pub" (including TLS PEM material),
//       then refreshes the cached cfg via bb_http_pub_set_cfg.
// Returns 204 on success; 400 on bad/missing body or invalid JSON.
#include "bb_http_pub_routes.h"
#include "bb_http_pub.h"
#include "bb_http.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_nv.h"
#include "bb_registry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bb_http_pub_routes";

#define BB_HTTPPUB_NVS_NS    "bb_http_pub"
#define BB_HTTPPUB_BODY_MAX  4096   // allow long PEM blocks in PATCH body

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
// GET /api/httppub
// ---------------------------------------------------------------------------

static bb_err_t httppub_get_handler(bb_http_request_t *req)
{
    char base[BB_HTTP_PUB_BASE_MAX]      = {0};
    char path_tmpl[BB_HTTP_PUB_PATH_MAX] = {0};
    char qos_str[4]                      = "1";
    char enabled_str[4]                  = "0";

    bb_nv_get_str(BB_HTTPPUB_NVS_NS, "base",      base,      sizeof(base),      "");
    bb_nv_get_str(BB_HTTPPUB_NVS_NS, "path_tmpl", path_tmpl, sizeof(path_tmpl), "");
    bb_nv_get_str(BB_HTTPPUB_NVS_NS, "qos",       qos_str,   sizeof(qos_str),   "1");
    bb_nv_get_str(BB_HTTPPUB_NVS_NS, "enabled",   enabled_str, sizeof(enabled_str), "0");

    // Check TLS PEM presence without exposing content.
    char ca_probe[8]   = {0};
    char cert_probe[8] = {0};
    char key_probe[8]  = {0};
    bb_nv_get_str(BB_HTTPPUB_NVS_NS, "tls_ca",   ca_probe,   sizeof(ca_probe),   "");
    bb_nv_get_str(BB_HTTPPUB_NVS_NS, "tls_cert", cert_probe, sizeof(cert_probe), "");
    bb_nv_get_str(BB_HTTPPUB_NVS_NS, "tls_key",  key_probe,  sizeof(key_probe),  "");

    bool ca_set   = (ca_probe[0]   != '\0');
    bool cert_set = (cert_probe[0] != '\0');
    bool key_set  = (key_probe[0]  != '\0');
    bool enabled  = (enabled_str[0] == '1');
    int  qos      = (int)(qos_str[0] - '0');
    if (qos < 0 || qos > 2) qos = 1;

    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;
    bb_http_resp_json_obj_set_str(&obj,  "base",      base);
    bb_http_resp_json_obj_set_str(&obj,  "path_tmpl", path_tmpl[0]
                                          ? path_tmpl
                                          : BB_HTTP_PUB_PATH_DEFAULT);
    bb_http_resp_json_obj_set_int(&obj,  "qos",       qos);
    bb_http_resp_json_obj_set_bool(&obj, "enabled",   enabled);
    bb_http_resp_json_obj_set_bool(&obj, "ca_set",    ca_set);
    bb_http_resp_json_obj_set_bool(&obj, "cert_set",  cert_set);
    bb_http_resp_json_obj_set_bool(&obj, "key_set",   key_set);
    return bb_http_resp_json_obj_end(&obj);
}

// ---------------------------------------------------------------------------
// PATCH /api/httppub
// ---------------------------------------------------------------------------

static bb_err_t httppub_patch_handler(bb_http_request_t *req)
{
    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0 || body_len > BB_HTTPPUB_BODY_MAX) {
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

    char tmp[BB_HTTPPUB_BODY_MAX + 1];

    if (bb_json_obj_get_string(parsed, "base",      tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_HTTPPUB_NVS_NS, "base", tmp);
    }
    if (bb_json_obj_get_string(parsed, "path_tmpl", tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_HTTPPUB_NVS_NS, "path_tmpl", tmp);
    }

    // TLS PEM fields stored in "bb_http_pub" NS for bb_tls_creds_resolve.
    if (bb_json_obj_get_string(parsed, "tls_ca",   tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_HTTPPUB_NVS_NS, "tls_ca", tmp);
    }
    if (bb_json_obj_get_string(parsed, "tls_cert", tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_HTTPPUB_NVS_NS, "tls_cert", tmp);
    }
    if (bb_json_obj_get_string(parsed, "tls_key",  tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_HTTPPUB_NVS_NS, "tls_key", tmp);
    }

    // Integer qos stored as single-char string.
    double qos_d;
    if (bb_json_obj_get_number(parsed, "qos", &qos_d)) {
        char qos_str[4] = {0};
        int qos = (int)qos_d;
        if (qos < 0) qos = 0;
        if (qos > 2) qos = 2;
        qos_str[0] = (char)('0' + qos);
        bb_nv_set_str(BB_HTTPPUB_NVS_NS, "qos", qos_str);
    }

    bool b;
    if (bb_json_obj_get_bool(parsed, "enabled", &b)) {
        bb_nv_set_str(BB_HTTPPUB_NVS_NS, "enabled", b ? "1" : "0");
    }

    bb_json_free(parsed);

    // Refresh cached cfg from updated NVS.
    bb_http_pub_init(NULL);

    bb_http_resp_no_content(req);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Route descriptors
// ---------------------------------------------------------------------------

static const bb_route_response_t s_httppub_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"base\":{\"type\":\"string\"},"
      "\"path_tmpl\":{\"type\":\"string\"},"
      "\"qos\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":2},"
      "\"enabled\":{\"type\":\"boolean\"},"
      "\"ca_set\":{\"type\":\"boolean\"},"
      "\"cert_set\":{\"type\":\"boolean\"},"
      "\"key_set\":{\"type\":\"boolean\"}},"
      "\"required\":[\"base\",\"path_tmpl\",\"qos\",\"enabled\","
                   "\"ca_set\",\"cert_set\",\"key_set\"]}",
      "HTTP publish configuration" },
    { 0 },
};

static const bb_route_response_t s_httppub_patch_responses[] = {
    { 204, NULL, NULL, "settings persisted" },
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "bad or missing request body" },
    { 0 },
};

static const bb_route_t s_httppub_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/httppub",
    .tag       = "httppub",
    .summary   = "Get HTTP publish configuration",
    .responses = s_httppub_get_responses,
    .handler   = httppub_get_handler,
};

static const bb_route_t s_httppub_patch_route = {
    .method               = BB_HTTP_PATCH,
    .path                 = "/api/httppub",
    .tag                  = "httppub",
    .summary              = "Persist HTTP publish configuration fields to NVS",
    .request_content_type = "application/json",
    .request_schema       =
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"base\":{\"type\":\"string\",\"description\":\"Base URL\"},"
        "\"path_tmpl\":{\"type\":\"string\","
                       "\"description\":\"Path template with {topic} and {qos}\"},"
        "\"tls_ca\":{\"type\":\"string\",\"description\":\"PEM CA certificate\"},"
        "\"tls_cert\":{\"type\":\"string\",\"description\":\"PEM client certificate\"},"
        "\"tls_key\":{\"type\":\"string\",\"description\":\"PEM client private key\"},"
        "\"qos\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":2},"
        "\"enabled\":{\"type\":\"boolean\"}}}",
    .responses = s_httppub_patch_responses,
    .handler   = httppub_patch_handler,
};

// ---------------------------------------------------------------------------
// Init (regular-tier)
// ---------------------------------------------------------------------------

bb_err_t bb_http_pub_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t rc = bb_http_register_described_route(server, &s_httppub_get_route);
    if (rc != BB_OK) return rc;
    rc = bb_http_register_described_route(server, &s_httppub_patch_route);
    if (rc != BB_OK) return rc;

    bb_log_i(TAG, "httppub routes registered");
    return BB_OK;
}

#if CONFIG_BB_HTTP_PUB_ROUTES_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_http_pub_routes, bb_http_pub_routes_init, 6);
#endif

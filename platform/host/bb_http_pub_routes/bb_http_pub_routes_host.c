// bb_http_pub_routes host twin — provides test hooks and a minimal route
// implementation backed by the in-memory NVS store.
//
// The actual GET/PATCH route logic mirrors the espidf source.  This file
// provides the host implementation and the BB_HTTP_PUB_ROUTES_TESTING hook
// surface (same pattern as bb_mqtt_routes, bb_fan_routes, bb_power_routes).
#include "bb_http_pub_routes.h"
#include "bb_http_pub.h"
#include "bb_nv.h"
#include "bb_http.h"
#include "bb_json.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define BB_HTTPPUB_NVS_NS    "bb_http_pub"
#define BB_HTTPPUB_BODY_MAX  4096

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

    if (bb_json_obj_get_string(parsed, "tls_ca",   tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_HTTPPUB_NVS_NS, "tls_ca", tmp);
    }
    if (bb_json_obj_get_string(parsed, "tls_cert", tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_HTTPPUB_NVS_NS, "tls_cert", tmp);
    }
    if (bb_json_obj_get_string(parsed, "tls_key",  tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_HTTPPUB_NVS_NS, "tls_key", tmp);
    }

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

    bb_http_resp_no_content(req);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Init (no-op on host — routes wired via test hooks)
// ---------------------------------------------------------------------------

bb_err_t bb_http_pub_routes_init(bb_http_handle_t server)
{
    (void)server;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_HTTP_PUB_ROUTES_TESTING

void bb_http_pub_routes_reset_for_test(void)
{
    bb_nv_host_str_store_reset();
}

bb_err_t bb_http_pub_routes_get_handler_for_test(bb_http_request_t *req)
{
    return httppub_get_handler(req);
}

bb_err_t bb_http_pub_routes_patch_handler_for_test(bb_http_request_t *req)
{
    return httppub_patch_handler(req);
}

#endif /* BB_HTTP_PUB_ROUTES_TESTING */

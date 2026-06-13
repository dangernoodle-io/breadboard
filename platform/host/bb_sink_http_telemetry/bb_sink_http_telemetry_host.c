// bb_sink_http_telemetry host twin — section get/patch logic + test hooks.
// Compiled on both host (test) and ESP-IDF (shared logic).
#include "bb_sink_http_telemetry.h"
#include "bb_sink_http.h"
#include "bb_nv.h"
#include "bb_json.h"
#include "bb_telemetry.h"
#include "bb_registry.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define BB_SINK_HTTP_NVS_NS    "bb_sink_http"
#define BB_SINK_HTTP_BODY_MAX  4096

// ---------------------------------------------------------------------------
// Section get
// ---------------------------------------------------------------------------

static void httppub_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;

    char base[BB_SINK_HTTP_BASE_MAX]      = {0};
    char path_tmpl[BB_SINK_HTTP_PATH_MAX] = {0};
    char qos_str[4]                      = "1";
    char enabled_str[4]                  = "0";

    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "base",      base,      sizeof(base),      "");
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "path_tmpl", path_tmpl, sizeof(path_tmpl), "");
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "qos",       qos_str,   sizeof(qos_str),   "1");
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "enabled",   enabled_str, sizeof(enabled_str), "0");

    char ca_probe[8]   = {0};
    char cert_probe[8] = {0};
    char key_probe[8]  = {0};
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "tls_ca",   ca_probe,   sizeof(ca_probe),   "");
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "tls_cert", cert_probe, sizeof(cert_probe), "");
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "tls_key",  key_probe,  sizeof(key_probe),  "");

    bool ca_set   = (ca_probe[0]   != '\0');
    bool cert_set = (cert_probe[0] != '\0');
    bool key_set  = (key_probe[0]  != '\0');
    bool enabled  = (enabled_str[0] == '1');
    int  qos      = (int)(qos_str[0] - '0');
    if (qos < 0 || qos > 2) qos = 1;

    bb_json_obj_set_string(section, "base",      base);
    bb_json_obj_set_string(section, "path_tmpl", path_tmpl[0]
                                      ? path_tmpl
                                      : BB_SINK_HTTP_PATH_DEFAULT);
    bb_json_obj_set_number(section, "qos",       (double)qos);
    bb_json_obj_set_bool  (section, "enabled",   enabled);
    bb_json_obj_set_bool  (section, "ca_set",    ca_set);
    bb_json_obj_set_bool  (section, "cert_set",  cert_set);
    bb_json_obj_set_bool  (section, "key_set",   key_set);
}

// ---------------------------------------------------------------------------
// Section patch
// ---------------------------------------------------------------------------

static bb_err_t httppub_section_patch(bb_json_t patch, void *ctx)
{
    (void)ctx;

    char tmp[BB_SINK_HTTP_BODY_MAX + 1];

    if (bb_json_obj_get_string(patch, "base",      tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "base", tmp);
    }
    if (bb_json_obj_get_string(patch, "path_tmpl", tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "path_tmpl", tmp);
    }
    if (bb_json_obj_get_string(patch, "tls_ca",   tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "tls_ca", tmp);
    }
    if (bb_json_obj_get_string(patch, "tls_cert", tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "tls_cert", tmp);
    }
    if (bb_json_obj_get_string(patch, "tls_key",  tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "tls_key", tmp);
    }

    double qos_d;
    if (bb_json_obj_get_number(patch, "qos", &qos_d)) {
        char qos_str[4] = {0};
        int qos = (int)qos_d;
        if (qos < 0) qos = 0;
        if (qos > 2) qos = 2;
        qos_str[0] = (char)('0' + qos);
        bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "qos", qos_str);
    }

    bool b;
    if (bb_json_obj_get_bool(patch, "enabled", &b)) {
        bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "enabled", b ? "1" : "0");
    }

    // Refresh cached cfg from updated NVS.
    bb_sink_http_init(NULL);

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bb_err_t bb_sink_http_telemetry_init(void)
{
    return bb_telemetry_register_section("http", httppub_section_get, httppub_section_patch, NULL);
}

#if CONFIG_BB_SINK_HTTP_TELEMETRY_AUTOREGISTER
BB_REGISTRY_REGISTER_PRE_HTTP(bb_sink_http_telemetry, bb_sink_http_telemetry_init);
#endif

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_SINK_HTTP_TELEMETRY_TESTING

void bb_sink_http_telemetry_reset_for_test(void)
{
    bb_nv_host_str_store_reset();
}

void bb_sink_http_telemetry_section_get_for_test(bb_json_t section, void *ctx)
{
    httppub_section_get(section, ctx);
}

bb_err_t bb_sink_http_telemetry_section_patch_for_test(bb_json_t patch, void *ctx)
{
    return httppub_section_patch(patch, ctx);
}

#endif /* BB_SINK_HTTP_TELEMETRY_TESTING */

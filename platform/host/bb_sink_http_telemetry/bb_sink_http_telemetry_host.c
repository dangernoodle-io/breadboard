// bb_sink_http_telemetry host twin — section get/patch logic + test hooks.
// Compiled on both host (test) and ESP-IDF (shared logic).
#include "bb_sink_http_telemetry.h"
#include "bb_sink_http.h"
#include "bb_nv.h"
#include "bb_json.h"
#include "bb_telemetry.h"
#include "bb_registry.h"
#include "bb_pub.h"
#include "bb_log.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Stable string ID used for the exclusive-sink arbiter.
#define BB_SINK_HTTP_EXCLUSIVE_ID  "http"

static const char *TAG = "bb_sink_http_telemetry";

#define BB_SINK_HTTP_NVS_NS    "bb_sink_http"
#define BB_SINK_HTTP_BODY_MAX  4096
#define HEADERS_NVS_KEY        "headers"
#define HEADERS_BUF_MAX        2048

// ---------------------------------------------------------------------------
// Section get
// ---------------------------------------------------------------------------

static void httppub_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;

    char base[BB_SINK_HTTP_BASE_MAX]              = {0};
    char path_tmpl[BB_SINK_HTTP_PATH_MAX]         = {0};
    char client_id[BB_SINK_HTTP_CLIENT_ID_MAX]    = {0};
    char qos_str[4]                              = "1";
    char enabled_str[4]                          = "0";

    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "base",      base,      sizeof(base),      "");
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "path_tmpl", path_tmpl, sizeof(path_tmpl), "");
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "client_id", client_id, sizeof(client_id), "");
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "qos",       qos_str,   sizeof(qos_str),   "1");
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "enabled",   enabled_str, sizeof(enabled_str), "0");

    bool ca_set   = bb_nv_exists(BB_SINK_HTTP_NVS_NS, "tls_ca");
    bool cert_set = bb_nv_exists(BB_SINK_HTTP_NVS_NS, "tls_cert");
    bool key_set  = bb_nv_exists(BB_SINK_HTTP_NVS_NS, "tls_key");
    bool enabled  = (enabled_str[0] == '1');
    int  qos      = (int)(qos_str[0] - '0');
    if (qos < 0 || qos > 2) qos = 1;

    bb_json_obj_set_string(section, "base",      base);
    bb_json_obj_set_string(section, "path_tmpl", path_tmpl[0]
                                      ? path_tmpl
                                      : BB_SINK_HTTP_PATH_DEFAULT);
    bb_json_obj_set_string(section, "client_id", client_id);
    bb_json_obj_set_number(section, "qos",       (double)qos);
    bb_json_obj_set_bool  (section, "enabled",   enabled);
    bb_json_obj_set_bool  (section, "ca_set",    ca_set);
    bb_json_obj_set_bool  (section, "cert_set",  cert_set);
    bb_json_obj_set_bool  (section, "key_set",   key_set);

    // Emit headers array — structured, with per-row secret masking.
    char *hbuf = calloc(1, HEADERS_BUF_MAX);
    if (!hbuf) {
        bb_json_obj_set_arr(section, "headers", bb_json_arr_new());
        return;
    }
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, HEADERS_NVS_KEY, hbuf, HEADERS_BUF_MAX, "");
    bb_sink_http_header_t *stored = calloc(BB_SINK_HTTP_HEADERS_MAX, sizeof(*stored));
    if (!stored) {
        free(hbuf);
        bb_json_obj_set_arr(section, "headers", bb_json_arr_new());
        return;
    }
    int n = bb_sink_http_parse_headers(hbuf, stored, BB_SINK_HTTP_HEADERS_MAX);
    free(hbuf);

    bb_json_t arr = bb_json_arr_new();
    for (int i = 0; i < n; i++) {
        bb_json_t entry = bb_json_obj_new();
        bb_json_obj_set_string(entry, "name",   stored[i].name);
        bb_json_obj_set_bool  (entry, "secret", stored[i].secret);
        if (!stored[i].secret) {
            bb_json_obj_set_string(entry, "value", stored[i].value);
        } else {
            bb_json_obj_set_bool(entry, "set", true);
        }
        bb_json_arr_append_obj(arr, entry);
    }
    free(stored);
    bb_json_obj_set_arr(section, "headers", arr);
}

// ---------------------------------------------------------------------------
// Section patch
// ---------------------------------------------------------------------------

static bb_err_t httppub_section_patch(bb_json_t patch, void *ctx)
{
    (void)ctx;

    char *tmp = malloc(BB_SINK_HTTP_BODY_MAX + 1);
    if (!tmp) return BB_ERR_NO_SPACE;

    if (bb_json_obj_get_string(patch, "base",      tmp, BB_SINK_HTTP_BODY_MAX + 1)) {
        bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "base", tmp);
    }
    if (bb_json_obj_get_string(patch, "path_tmpl", tmp, BB_SINK_HTTP_BODY_MAX + 1)) {
        bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "path_tmpl", tmp);
    }
    if (bb_json_obj_get_string(patch, "client_id", tmp, BB_SINK_HTTP_BODY_MAX + 1)) {
        bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "client_id", tmp);
    }
    if (bb_json_obj_get_string(patch, "tls_ca",   tmp, BB_SINK_HTTP_BODY_MAX + 1)) {
        bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "tls_ca", tmp);
    }
    if (bb_json_obj_get_string(patch, "tls_cert", tmp, BB_SINK_HTTP_BODY_MAX + 1)) {
        bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "tls_cert", tmp);
    }
    if (bb_json_obj_get_string(patch, "tls_key",  tmp, BB_SINK_HTTP_BODY_MAX + 1)) {
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
        if (b) {
            // Acquire the exclusive slot before persisting enabled=true.
            bb_err_t arc = bb_pub_exclusive_acquire(BB_SINK_HTTP_EXCLUSIVE_ID);
            if (arc != BB_OK) {
                bb_log_w(TAG, "enable rejected: exclusive sink conflict");
                free(tmp);
                return arc;
            }
        } else {
            bb_pub_exclusive_release(BB_SINK_HTTP_EXCLUSIVE_ID);
        }
        bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "enabled", b ? "1" : "0");
    }

    // Process headers array if present.
    bb_json_t harr = bb_json_obj_get_item(patch, "headers");
    if (harr && bb_json_item_is_array(harr)) {
        int patch_count = bb_json_arr_size(harr);
        if (patch_count > BB_SINK_HTTP_HEADERS_MAX) patch_count = BB_SINK_HTTP_HEADERS_MAX;

        // Build patch_entry array from JSON.
        bb_sink_http_patch_entry_t *patch_entries =
            calloc(BB_SINK_HTTP_HEADERS_MAX, sizeof(*patch_entries));
        if (!patch_entries) {
            free(tmp);
            return BB_ERR_NO_SPACE;
        }
        int valid_patch = 0;

        for (int i = 0; i < patch_count; i++) {
            bb_json_t item = bb_json_arr_get_item(harr, i);
            if (!item) continue;

            bb_sink_http_patch_entry_t *pe = &patch_entries[valid_patch];
            if (!bb_json_obj_get_string(item, "name", pe->name, sizeof(pe->name))) {
                continue;  // name required
            }
            if (!bb_sink_http_header_name_valid(pe->name)) continue;

            bool secret_field = false;
            bb_json_obj_get_bool(item, "secret", &secret_field);
            pe->secret = secret_field;

            char val_tmp[BB_SINK_HTTP_HEADER_VALUE_MAX] = {0};
            if (bb_json_obj_get_string(item, "value", val_tmp, sizeof(val_tmp))) {
                pe->value_present = true;
                strncpy(pe->value, val_tmp, sizeof(pe->value) - 1);
                pe->value[sizeof(pe->value) - 1] = '\0';
                if (!bb_sink_http_header_value_valid(pe->value)) continue;
            } else {
                pe->value_present = false;
                pe->value[0] = '\0';
            }

            valid_patch++;
        }

        // Load existing headers from NVS for secret-preserve merge.
        char *hbuf = calloc(1, HEADERS_BUF_MAX);
        if (!hbuf) {
            free(patch_entries);
            free(tmp);
            return BB_ERR_NO_SPACE;
        }
        bb_nv_get_str(BB_SINK_HTTP_NVS_NS, HEADERS_NVS_KEY, hbuf, HEADERS_BUF_MAX, "");
        bb_sink_http_header_t *existing =
            calloc(BB_SINK_HTTP_HEADERS_MAX, sizeof(*existing));
        if (!existing) {
            free(hbuf);
            free(patch_entries);
            free(tmp);
            return BB_ERR_NO_SPACE;
        }
        int existing_count = bb_sink_http_parse_headers(hbuf, existing, BB_SINK_HTTP_HEADERS_MAX);
        free(hbuf);

        // Merge.
        bb_sink_http_header_t *merged =
            calloc(BB_SINK_HTTP_HEADERS_MAX, sizeof(*merged));
        if (!merged) {
            free(existing);
            free(patch_entries);
            free(tmp);
            return BB_ERR_NO_SPACE;
        }
        int merged_count = bb_sink_http_merge_headers(
            patch_entries, valid_patch,
            existing, existing_count,
            merged, BB_SINK_HTTP_HEADERS_MAX);
        free(existing);
        free(patch_entries);

        // Serialize and persist.
        char *out_buf = calloc(1, HEADERS_BUF_MAX);
        if (!out_buf) {
            free(merged);
            free(tmp);
            return BB_ERR_NO_SPACE;
        }
        bb_sink_http_serialize_headers(merged, merged_count, out_buf, HEADERS_BUF_MAX);
        free(merged);
        bb_nv_set_str(BB_SINK_HTTP_NVS_NS, HEADERS_NVS_KEY, out_buf);
        free(out_buf);
    }

    free(tmp);

    // Refresh cached cfg from updated NVS.
    bb_sink_http_init(NULL);

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bb_err_t bb_sink_http_telemetry_init(void)
{
    // Boot-time precedence: if this sink is NVS-enabled, try to acquire the
    // exclusive slot. If the slot is already held (MQTT enabled and registered
    // first), log a warning and write enabled=0 back to NVS — MQTT wins.
    char enabled_str[4] = "0";
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "enabled", enabled_str, sizeof(enabled_str), "0");
    if (enabled_str[0] == '1') {
        bb_err_t arc = bb_pub_exclusive_acquire(BB_SINK_HTTP_EXCLUSIVE_ID);
        if (arc != BB_OK) {
            bb_log_w(TAG, "boot: mqtt and http both enabled in NVS — "
                     "disabling http sink (mqtt wins)");
            bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "enabled", "0");
        }
    }
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
    bb_pub_exclusive_reset();
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

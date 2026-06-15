// bb_nv_delete_routes — NVS delete HTTP routes (B1-290).
//
// Portable: no ESP-IDF-specific includes. Compiled on both ESP-IDF and host
// (for unit tests). All NVS access goes through the portable bb_nv_erase /
// bb_nv_erase_namespace APIs.
//
// Endpoint: DELETE /api/nvs with a JSON request body.
//
// Body schema:
//   {
//     "namespace": <string | array-of-strings>,  // required
//     "key":       <string>,                      // optional; forbidden with array namespace
//     "confirm":   true,                          // required; else 412
//     "wipe_wifi": true                           // required when namespace is/includes "bb_cfg"
//   }
//
// Guard policy:
//   - "confirm": true required (412 without it).
//   - "namespace" = "bb_cfg" (or an array that includes "bb_cfg") additionally
//     requires "wipe_wifi": true because bb_cfg holds wifi credentials.
//     Note: if CONFIG_BB_NV_CREDS_RTC_BACKUP is enabled, credentials are
//     automatically restored from the RTC mirror on the next boot if the
//     mirror is valid.
//   - "key" with an array namespace returns 400 (ambiguous).
//
// Responses:
//   200 {"deleted": [...]}  — list of namespaces (and key) cleared
//   400 missing/invalid namespace, or key+array combo
//   412 missing confirm / missing wipe_wifi when bb_cfg in scope
//   500 NVS erase operation failed

#include "bb_nv_delete_routes.h"
#include "bb_http.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_nv.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "bb_nv_del";

#define BB_NV_DELETE_BODY_MAX 1024

// ---------------------------------------------------------------------------
// Error helpers
// ---------------------------------------------------------------------------

static void send_412(bb_http_request_t *req, const char *msg)
{
    bb_http_resp_set_status(req, 412);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "error", msg);
    bb_http_resp_json_obj_end(&obj);
}

static void send_400(bb_http_request_t *req, const char *msg)
{
    bb_http_resp_set_status(req, 400);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "error", msg);
    bb_http_resp_json_obj_end(&obj);
}

static void send_500(bb_http_request_t *req, const char *msg)
{
    bb_http_resp_set_status(req, 500);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "error", msg);
    bb_http_resp_json_obj_end(&obj);
}

// ---------------------------------------------------------------------------
// DELETE /api/nvs — JSON body
// ---------------------------------------------------------------------------

static bb_err_t nvs_delete_handler(bb_http_request_t *req)
{
    /* Read body. */
    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0 || body_len > BB_NV_DELETE_BODY_MAX) {
        send_400(req, "missing or oversized body");
        return BB_ERR_INVALID_ARG;
    }

    char *body = malloc((size_t)body_len + 1);
    if (!body) {
        send_500(req, "out of memory");
        return BB_ERR_NO_SPACE;
    }

    int n = bb_http_req_recv(req, body, (size_t)(body_len + 1));
    if (n < 0) {
        free(body);
        send_400(req, "body read failed");
        return BB_ERR_INVALID_ARG;
    }
    body[n] = '\0';

    bb_json_t parsed = bb_json_parse(body, (size_t)n);
    free(body);
    if (!parsed) {
        send_400(req, "invalid JSON");
        return BB_ERR_INVALID_ARG;
    }

    /* --- confirm: true required --- */
    bool confirm = false;
    if (!bb_json_obj_get_bool(parsed, "confirm", &confirm) || !confirm) {
        bb_json_free(parsed);
        send_412(req, "pass \"confirm\": true to confirm NVS deletion");
        return BB_ERR_INVALID_STATE;
    }

    /* --- namespace: string or array-of-strings --- */
    bb_json_t ns_item = bb_json_obj_get_item(parsed, "namespace");
    if (!ns_item) {
        bb_json_free(parsed);
        send_400(req, "missing required \"namespace\" field");
        return BB_ERR_INVALID_ARG;
    }

    bool ns_is_array = bb_json_item_is_array(ns_item);
    bool ns_is_str   = bb_json_item_is_string(ns_item);
    if (!ns_is_array && !ns_is_str) {
        bb_json_free(parsed);
        send_400(req, "\"namespace\" must be a string or array of strings");
        return BB_ERR_INVALID_ARG;
    }

    /* --- optional key: forbidden with array namespace --- */
    char key[16] = {0};
    bool has_key = bb_json_obj_get_string(parsed, "key", key, sizeof(key))
                   && key[0] != '\0';

    if (has_key && ns_is_array) {
        bb_json_free(parsed);
        send_400(req, "\"key\" is ambiguous when \"namespace\" is an array; use a single namespace string");
        return BB_ERR_INVALID_ARG;
    }

    /* --- wipe_wifi guard for bb_cfg --- */
    bool needs_wipe_wifi = false;
    if (ns_is_str) {
        const char *ns_val = bb_json_item_get_string(ns_item);
        if (ns_val && strcmp(ns_val, "bb_cfg") == 0) {
            needs_wipe_wifi = true;
        }
    } else {
        /* array: check each element */
        int cnt = bb_json_arr_size(ns_item);
        for (int i = 0; i < cnt; i++) {
            bb_json_t el = bb_json_arr_get_item(ns_item, i);
            if (el && bb_json_item_is_string(el)) {
                const char *s = bb_json_item_get_string(el);
                if (s && strcmp(s, "bb_cfg") == 0) {
                    needs_wipe_wifi = true;
                    break;
                }
            }
        }
    }

    if (needs_wipe_wifi) {
        bool wipe_wifi = false;
        if (!bb_json_obj_get_bool(parsed, "wipe_wifi", &wipe_wifi) || !wipe_wifi) {
            bb_json_free(parsed);
            send_412(req,
                "namespace bb_cfg contains wifi credentials; also pass "
                "\"wipe_wifi\": true to confirm wifi credential erasure. "
                "Note: if CONFIG_BB_NV_CREDS_RTC_BACKUP is enabled, "
                "credentials are restored from RTC on next boot.");
            return BB_ERR_INVALID_STATE;
        }
    }

    /* --- Perform erase(s) --- */
    bb_err_t overall = BB_OK;

    /* Build the response "deleted" array as we go. */
    bb_json_t deleted_arr = bb_json_arr_new();
    if (!deleted_arr) {
        bb_json_free(parsed);
        send_500(req, "out of memory");
        return BB_ERR_NO_SPACE;
    }

    if (ns_is_str) {
        const char *ns = bb_json_item_get_string(ns_item);
        bb_err_t err;
        if (has_key) {
            err = bb_nv_erase(ns, key);
            bb_log_i(TAG, "DELETE /api/nvs ns=%s key=%s -> %d", ns, key, (int)err);
        } else {
            err = bb_nv_erase_namespace(ns);
            bb_log_i(TAG, "DELETE /api/nvs ns=%s (all) -> %d", ns, (int)err);
        }
        if (err != BB_OK) {
            overall = err;
        } else {
            bb_json_arr_append_string(deleted_arr, ns);
        }
    } else {
        /* array of namespaces — erase each in turn; continue on individual errors */
        int cnt = bb_json_arr_size(ns_item);
        for (int i = 0; i < cnt; i++) {
            bb_json_t el = bb_json_arr_get_item(ns_item, i);
            if (!el || !bb_json_item_is_string(el)) continue;
            const char *ns = bb_json_item_get_string(el);
            if (!ns || ns[0] == '\0') continue;
            bb_err_t err = bb_nv_erase_namespace(ns);
            bb_log_i(TAG, "DELETE /api/nvs ns=%s (all) -> %d", ns, (int)err);
            if (err != BB_OK) {
                if (overall == BB_OK) overall = err;
            } else {
                bb_json_arr_append_string(deleted_arr, ns);
            }
        }
    }

    bb_json_free(parsed);

    if (overall != BB_OK) {
        bb_json_free(deleted_arr);
        send_500(req, "one or more NVS erase operations failed");
        return overall;
    }

    /* 200 {"deleted": [...]} */
    bb_json_t resp = bb_json_obj_new();
    if (!resp) {
        bb_json_free(deleted_arr);
        send_500(req, "out of memory");
        return BB_ERR_NO_SPACE;
    }
    bb_json_obj_set_arr(resp, "deleted", deleted_arr);
    if (has_key) {
        bb_json_obj_set_string(resp, "key", key);
    }

    char *json = bb_json_serialize(resp);
    bb_json_free(resp);
    if (!json) {
        send_500(req, "serialize failed");
        return BB_ERR_NO_SPACE;
    }

    bb_http_resp_set_type(req, "application/json");
    bb_http_resp_send_chunk(req, json, (int)strlen(json));
    bb_http_resp_send_chunk(req, NULL, 0);
    bb_json_free_str(json);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Route descriptor
// ---------------------------------------------------------------------------

static const bb_route_response_t s_nvs_delete_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"deleted\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
      "\"key\":{\"type\":\"string\"}},"
      "\"required\":[\"deleted\"]}",
      "deletion successful; 'key' present only when a single key was erased" },
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "missing/invalid namespace, or key+array-namespace combo" },
    { 412, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "missing confirm:true / missing wipe_wifi:true for bb_cfg" },
    { 500, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "NVS erase operation failed" },
    { 0 },
};

static const bb_route_t s_nvs_delete_route = {
    .method           = BB_HTTP_DELETE,
    .path             = "/api/nvs",
    .tag              = "diag",
    .summary          = "Delete one NVS key or one/multiple namespaces via JSON body. "
                        "Requires {\"confirm\":true}. For namespace 'bb_cfg' also requires "
                        "{\"wipe_wifi\":true} (contains wifi credentials; RTC backup may restore "
                        "them on next boot if CONFIG_BB_NV_CREDS_RTC_BACKUP is enabled). "
                        "Use an array namespace to reset multiple namespaces in one call "
                        "(e.g. the 'reset telemetry' case: [\"bb_mqtt\",\"bb_sink_http\",\"bb_pub\"]). "
                        "\"key\" is forbidden when namespace is an array.",
    .request_schema   = "{\"type\":\"object\","
                        "\"properties\":{"
                        "\"namespace\":{\"oneOf\":[{\"type\":\"string\"},{\"type\":\"array\",\"items\":{\"type\":\"string\"}}]},"
                        "\"key\":{\"type\":\"string\"},"
                        "\"confirm\":{\"type\":\"boolean\"},"
                        "\"wipe_wifi\":{\"type\":\"boolean\"}},"
                        "\"required\":[\"namespace\",\"confirm\"]}",
    .request_content_type = "application/json",
    .responses        = s_nvs_delete_responses,
    .parameters       = NULL,
    .parameters_count = 0,
    .handler          = nvs_delete_handler,
};

// ---------------------------------------------------------------------------
// Init: register the single route
// ---------------------------------------------------------------------------

bb_err_t bb_nv_delete_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t err = bb_http_register_described_route(server, &s_nvs_delete_route);
    if (err != BB_OK) return err;

    bb_log_i(TAG, "NVS delete route registered");
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Testing exposure (host unit tests)
// ---------------------------------------------------------------------------

#ifdef BB_NV_DELETE_ROUTES_TESTING
bb_err_t bb_nv_delete_handler_for_test(bb_http_request_t *req)
{
    return nvs_delete_handler(req);
}
#endif /* BB_NV_DELETE_ROUTES_TESTING */

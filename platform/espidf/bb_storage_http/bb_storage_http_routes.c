// bb_storage_http_routes — DELETE /api/diag/storage handler (B1-757).
//
// Portable: no ESP-IDF-specific includes. Compiled on both ESP-IDF and host
// (for unit tests). All storage access goes through the portable
// bb_storage_erase / bb_storage_erase_namespace facade — this file never
// calls a backend-specific (e.g. bb_storage_nvs_*) function directly, which
// is the whole point of rehoming this route off bb_nv.

#include "bb_storage_http.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_body.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_mem.h"
#include "bb_settings.h"
#include "bb_str.h"
#include "bb_system.h"

#ifdef ESP_PLATFORM
#include "bb_clock.h"
#include "bb_timer.h"
#include "esp_system.h"
#endif

#include <stdlib.h>
#include <string.h>

static const char *TAG = "bb_storage_http";

#define BB_STORAGE_HTTP_DELETE_BODY_MAX        1024
#define BB_STORAGE_HTTP_BACKEND_MAX            16
#define BB_STORAGE_HTTP_FACTORY_RESET_BODY_MAX 128

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

static void send_501(bb_http_request_t *req, const char *msg)
{
    bb_http_resp_set_status(req, 501);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "error", msg);
    bb_http_resp_json_obj_end(&obj);
}

// ---------------------------------------------------------------------------
// Bounded string-field getter that REJECTS an oversized value instead of
// silently truncating it (bb_json_obj_get_string() truncates+NUL-terminates,
// which is unsafe for fields like "backend"/"key" that select a destructive
// target -- a truncated value could collide with a real name and erase the
// wrong store). Reads the field via bb_json_obj_get_item()/
// bb_json_item_get_string(), whose pointer is into the untouched parse tree
// (no copy, no truncation), so we can check the real length before copying.
// ---------------------------------------------------------------------------

typedef enum {
    FIELD_ABSENT,    // not present, not a string, or NULL string value
    FIELD_OK,        // present and copied into out
    FIELD_TOO_LONG,  // present but does not fit in out_size
} field_result_t;

static field_result_t get_string_field_checked(bb_json_t obj, const char *key, char *out, size_t out_size)
{
    bb_json_t item = bb_json_obj_get_item(obj, key);
    if (!item || !bb_json_item_is_string(item)) return FIELD_ABSENT;
    const char *s = bb_json_item_get_string(item);
    if (!s) return FIELD_ABSENT;
    size_t len = strlen(s);
    if (len >= out_size) return FIELD_TOO_LONG;
    memcpy(out, s, len);
    out[len] = '\0';
    return FIELD_OK;
}

// ---------------------------------------------------------------------------
// DELETE /api/diag/storage — JSON body
// ---------------------------------------------------------------------------

static bb_err_t storage_delete_handler(bb_http_request_t *req)
{
    /* Read body. */
    char *body = NULL;
    int   n    = 0;
    bb_err_t brc = bb_http_req_recv_body_alloc(req, BB_STORAGE_HTTP_DELETE_BODY_MAX, &body, &n);
    if (brc == BB_ERR_INVALID_ARG) {
        send_400(req, "missing or oversized body");
        return brc;
    }
    if (brc == BB_ERR_NO_SPACE) {
        // body_len > max_bytes → 400; OOM → 500
        int bl = bb_http_req_body_len(req);
        if (bl > BB_STORAGE_HTTP_DELETE_BODY_MAX) {
            send_400(req, "missing or oversized body");
        } else {
            send_500(req, "out of memory");
        }
        return brc;
    }
    if (brc != BB_OK) {
        send_400(req, "body read failed");
        return brc;
    }

    bb_json_t parsed = bb_json_parse(body, (size_t)n);
    bb_mem_free(body);
    if (!parsed) {
        send_400(req, "invalid JSON");
        return BB_ERR_INVALID_ARG;
    }

    /* --- confirm: true required --- */
    bool confirm = false;
    if (!bb_json_obj_get_bool(parsed, "confirm", &confirm) || !confirm) {
        bb_json_free(parsed);
        send_412(req, "pass \"confirm\": true to confirm storage deletion");
        return BB_ERR_INVALID_STATE;
    }

    /* --- backend: optional, defaults to "nvs"; oversized value is rejected
     * (400), never silently truncated -- see get_string_field_checked(). --- */
    char backend[BB_STORAGE_HTTP_BACKEND_MAX] = {0};
    field_result_t backend_res = get_string_field_checked(parsed, "backend", backend, sizeof(backend));
    if (backend_res == FIELD_TOO_LONG) {
        bb_json_free(parsed);
        send_400(req, "\"backend\" value too long");
        return BB_ERR_INVALID_ARG;
    }
    if (backend_res != FIELD_OK || backend[0] == '\0') {
        bb_strlcpy(backend, "nvs", sizeof(backend));
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

    /* --- optional key: forbidden with array namespace; oversized value is
     * rejected (400), never silently truncated -- see
     * get_string_field_checked(). --- */
    char key[16] = {0};
    field_result_t key_res = get_string_field_checked(parsed, "key", key, sizeof(key));
    if (key_res == FIELD_TOO_LONG) {
        bb_json_free(parsed);
        send_400(req, "\"key\" value too long");
        return BB_ERR_INVALID_ARG;
    }
    bool has_key = (key_res == FIELD_OK) && key[0] != '\0';

    if (has_key && ns_is_array) {
        bb_json_free(parsed);
        send_400(req, "\"key\" is ambiguous when \"namespace\" is an array; use a single namespace string");
        return BB_ERR_INVALID_ARG;
    }

    /* --- wipe_wifi guard: ASK bb_settings which namespace holds wifi creds
     * rather than comparing against a copied "bb_cfg" literal -- a copy
     * would silently go stale if bb_settings' namespace ever changed. --- */
    bool needs_wipe_wifi = false;
    if (ns_is_str) {
        const char *ns_val = bb_json_item_get_string(ns_item);
        if (ns_val && bb_settings_ns_is_wifi_creds(ns_val)) {
            needs_wipe_wifi = true;
        }
    } else {
        /* array: check each element */
        int cnt = bb_json_arr_size(ns_item);
        for (int i = 0; i < cnt; i++) {
            bb_json_t el = bb_json_arr_get_item(ns_item, i);
            if (el && bb_json_item_is_string(el)) {
                const char *s = bb_json_item_get_string(el);
                if (s && bb_settings_ns_is_wifi_creds(s)) {
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
                "this namespace contains wifi credentials; also pass "
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
            bb_storage_addr_t addr = { .backend = backend, .ns_or_dir = ns, .key = key };
            err = bb_storage_erase(&addr);
            bb_log_i(TAG, "DELETE /api/diag/storage backend=%s ns=%s key=%s -> %d",
                     backend, ns, key, (int)err);
        } else {
            err = bb_storage_erase_namespace(backend, ns);
            bb_log_i(TAG, "DELETE /api/diag/storage backend=%s ns=%s (all) -> %d",
                     backend, ns, (int)err);
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
            bb_err_t err = bb_storage_erase_namespace(backend, ns);
            bb_log_i(TAG, "DELETE /api/diag/storage backend=%s ns=%s (all) -> %d",
                     backend, ns, (int)err);
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
        if (overall == BB_ERR_UNSUPPORTED) {
            send_501(req, "backend does not support namespace-level erase");
        } else {
            send_500(req, "one or more storage erase operations failed");
        }
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

static const bb_route_response_t s_storage_delete_responses[] = {
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
      "missing confirm:true / missing wipe_wifi:true for the wifi-creds namespace" },
    { 500, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "storage erase operation failed" },
    { 501, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "backend does not support namespace-level erase" },
    { 0 },
};

static const bb_route_t s_storage_delete_route = {
    .method           = BB_HTTP_DELETE,
    .path             = "/api/diag/storage",
    .tag              = "diag",
    .summary          = "Delete one storage key or one/multiple namespaces via JSON body. "
                        "Requires {\"confirm\":true}. Works against any registered bb_storage "
                        "backend (\"backend\", default \"nvs\"). For the wifi-creds namespace "
                        "(bb_settings-owned) also requires {\"wipe_wifi\":true} (contains wifi "
                        "credentials; RTC backup may restore them on next boot if "
                        "CONFIG_BB_NV_CREDS_RTC_BACKUP is enabled). Use an array namespace to "
                        "reset multiple namespaces in one call (e.g. [\"bb_mqtt\",\"bb_udp\",\"bb_tcp\"]). "
                        "\"key\" is forbidden when namespace is an array.",
    .request_schema   = "{\"type\":\"object\","
                        "\"properties\":{"
                        "\"backend\":{\"type\":\"string\"},"
                        "\"namespace\":{\"oneOf\":[{\"type\":\"string\"},{\"type\":\"array\",\"items\":{\"type\":\"string\"}}]},"
                        "\"key\":{\"type\":\"string\"},"
                        "\"confirm\":{\"type\":\"boolean\"},"
                        "\"wipe_wifi\":{\"type\":\"boolean\"}},"
                        "\"required\":[\"namespace\",\"confirm\"]}",
    .request_content_type = "application/json",
    .responses        = s_storage_delete_responses,
    .parameters       = NULL,
    .parameters_count = 0,
    .handler          = storage_delete_handler,
};

// ---------------------------------------------------------------------------
// POST /api/diag/factory-reset — whole-"nvs"-backend erase + reboot
// (B1-960, rehomed off bb_nv's POST /api/factory-reset /
// bb_nv_factory_reset_routes_init). Request schema PRESERVED exactly from
// the old bb_nv route: {"confirm":"factory-reset"} exact-string match.
//
// Gated behind CONFIG_BB_STORAGE_HTTP_FACTORY_RESET (default n — see
// bb_storage_http.h). Mirrors the deleted bb_nv_routes.c's
// `#if CONFIG_BB_NV_FACTORY_RESET` posture: destructive routes are opt-in.
// ---------------------------------------------------------------------------

#if defined(CONFIG_BB_STORAGE_HTTP_FACTORY_RESET) && CONFIG_BB_STORAGE_HTTP_FACTORY_RESET

#ifdef ESP_PLATFORM
static void factory_reset_reboot_work_fn(void *arg)
{
    (void)arg;
    uint32_t uptime_s = (uint32_t)(bb_clock_now_ms64() / 1000ULL);
    /* epoch_s=0: no bb_ntp dependency here (would create an unwanted edge);
     * the boot-side reader treats epoch_s=0 as "unknown/unsynced" per the
     * record contract. */
    bb_err_t rc = bb_system_reboot_record_save(BB_RESET_SRC_FACTORY_RESET, NULL, 0, uptime_s);
    if (rc == BB_ERR_INVALID_ARG) {
        bb_log_w(TAG, "factory_reset: record encode failed, rebooting without reason");
    } else if (rc != BB_OK) {
        bb_log_w(TAG, "factory_reset: NVS persist failed: %d", (int)rc);
    }
    esp_restart();
}
#endif /* ESP_PLATFORM */

static bb_err_t factory_reset_handler(bb_http_request_t *req)
{
    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0 || body_len > BB_STORAGE_HTTP_FACTORY_RESET_BODY_MAX) {
        send_400(req, "missing or oversized body");
        return BB_ERR_INVALID_ARG;
    }

    char body[BB_STORAGE_HTTP_FACTORY_RESET_BODY_MAX];
    int n = bb_http_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        send_400(req, "read failed");
        return BB_ERR_INVALID_ARG;
    }
    body[n] = '\0';

    bb_json_t parsed = bb_json_parse(body, (size_t)n);
    if (!parsed) {
        send_400(req, "invalid JSON");
        return BB_ERR_INVALID_ARG;
    }

    char confirm[32];
    confirm[0] = '\0';
    bb_json_obj_get_string(parsed, "confirm", confirm, sizeof(confirm));
    bb_json_free(parsed);

    if (strcmp(confirm, "factory-reset") != 0) {
        send_400(req, "confirm field must be \"factory-reset\"");
        return BB_ERR_INVALID_ARG;
    }

    bb_err_t err = bb_storage_erase_all("nvs");
    if (err != BB_OK) {
        if (err == BB_ERR_UNSUPPORTED) {
            send_501(req, "backend does not support whole-backend erase");
        } else {
            send_500(req, "factory reset failed");
        }
        return err;
    }

    /* Invalidate the shared RTC mirror so the restore-heal path does NOT
     * re-populate credentials on next boot. Best-effort/fail-open, same
     * posture as every other mirror clear in this codebase: an
     * unregistered "rtc" backend (RTC-backup Kconfig-disabled builds, or a
     * composer that never registered bb_storage_rtc) is harmless here.
     *
     * On device this stays gated on CONFIG_BB_NV_CREDS_RTC_BACKUP (the
     * mirror only exists when that feature is on). On host there is no
     * sdkconfig.h / CONFIG_ bridge at all, so the call is unconditional —
     * same posture as the deleted bb_nv_config_factory_reset() host stub,
     * which called this unconditionally and was covered by
     * test_nv_factory_reset_invalidates_rtc_mirror. Keeping it unconditional
     * on host (rather than compiling it out) is what makes this
     * host-testable (B1-935 stale-creds-survive-factory-reset class). */
#if !defined(ESP_PLATFORM) || defined(CONFIG_BB_NV_CREDS_RTC_BACKUP)
    bb_settings_wifi_rtc_mirror_clear();
#endif

    bb_http_resp_set_status(req, 202);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "status", "factory_reset_accepted");
    bb_http_resp_json_obj_set_bool(&obj, "reboot", true);
    bb_http_resp_json_obj_end(&obj);

#ifdef ESP_PLATFORM
    static bb_oneshot_timer_t s_reset_timer = NULL;
    if (!s_reset_timer) {
        bb_timer_deferred_oneshot_create(factory_reset_reboot_work_fn, NULL,
                                         "bb_storage_http_factory_reset", &s_reset_timer);
    }
    bb_timer_oneshot_stop(s_reset_timer);
    bb_timer_oneshot_start(s_reset_timer, 500 * 1000); /* 500 ms — lets HTTP 202 flush */
#endif /* ESP_PLATFORM */

    return BB_OK;
}

static const bb_route_response_t s_factory_reset_responses[] = {
    { 202, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"status\":{\"type\":\"string\"},"
      "\"reboot\":{\"type\":\"boolean\"}},"
      "\"required\":[\"status\",\"reboot\"]}",
      "factory reset accepted; device will reboot after ~500 ms" },
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "missing or invalid confirmation body" },
    { 500, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "factory reset (nvs erase) failed" },
    { 501, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "backend does not support whole-backend erase" },
    { 0 },
};

static const bb_route_t s_factory_reset_route = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/diag/factory-reset",
    .tag                  = "diag",
    .summary              = "Erase the whole \"nvs\" bb_storage backend and reboot to factory defaults",
    .request_content_type = "application/json",
    .request_schema       = "{\"type\":\"object\","
                            "\"properties\":{\"confirm\":{\"type\":\"string\"}},"
                            "\"required\":[\"confirm\"]}",
    .responses            = s_factory_reset_responses,
    .handler              = factory_reset_handler,
};

bb_err_t bb_storage_http_factory_reset_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t err = bb_http_register_described_route(server, &s_factory_reset_route);
    if (err != BB_OK) return err;

    bb_log_i(TAG, "factory reset route registered");
    return BB_OK;
}

#ifdef BB_STORAGE_HTTP_TESTING
bb_err_t bb_storage_http_factory_reset_handler_for_test(bb_http_request_t *req)
{
    return factory_reset_handler(req);
}
#endif /* BB_STORAGE_HTTP_TESTING */

#endif /* defined(CONFIG_BB_STORAGE_HTTP_FACTORY_RESET) && CONFIG_BB_STORAGE_HTTP_FACTORY_RESET */

// ---------------------------------------------------------------------------
// Init: register the single (always-on) DELETE route
// ---------------------------------------------------------------------------

bb_err_t bb_storage_http_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t err = bb_http_register_described_route(server, &s_storage_delete_route);
    if (err != BB_OK) return err;

    bb_log_i(TAG, "storage delete route registered");
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Testing exposure (host unit tests) — non-factory-reset handler
// ---------------------------------------------------------------------------

#ifdef BB_STORAGE_HTTP_TESTING
bb_err_t bb_storage_http_delete_handler_for_test(bb_http_request_t *req)
{
    return storage_delete_handler(req);
}
#endif /* BB_STORAGE_HTTP_TESTING */

#include "bb_diag.h"
#include "bb_cache.h"
#include "bb_clock.h"
#include "bb_diag_event_priv.h"
#include "bb_event.h"
#include "bb_event_routes.h"
#include "bb_http.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_net_health.h"
#include "bb_openapi.h"
#include "bb_nv_delete_routes.h"
#include "bb_ota_validator.h"
#include "bb_init.h"
#include "bb_system.h"
#include "bb_mem.h"
#include "bb_task_registry.h"

#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
#include "esp_core_dump.h"
#include "esp_partition.h"
#include <stdio.h>
#endif

#include "lwip/tcpip.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/priv/tcp_priv.h"

static const char *TAG = "bb_diag_routes";

static bb_event_topic_t s_boot_topic = NULL;

static void diag_boot_publish(void)
{
    if (s_boot_topic == NULL) return;
    bb_diag_boot_snap_t snap;
    const char *rr = bb_system_reset_reason_str(bb_system_get_reset_reason());
    size_t rr_len = strlen(rr);
    if (rr_len >= sizeof(snap.reset_reason)) rr_len = sizeof(snap.reset_reason) - 1;
    memcpy(snap.reset_reason, rr, rr_len);
    snap.reset_reason[rr_len] = '\0';
    snap.wdt_resets        = bb_diag_abnormal_reset_count();
    snap.panic_available   = bb_diag_panic_available();
    snap.panic_boots_since = snap.panic_available ? bb_diag_panic_boots_since() : 0;
    snap.pending_verify    = !bb_ota_is_validated();
    snap.rolled_back       = bb_ota_rolled_back();
    bb_cache_update(BB_DIAG_BOOT_TOPIC, &snap);
    bb_cache_post(BB_DIAG_BOOT_TOPIC);
}

static bb_err_t panic_get_handler(bb_http_request_t *req)
{
    bool available     = bb_diag_panic_available();
    bool coredump_avail = bb_diag_panic_coredump_available();

    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;

    bb_http_resp_json_obj_set_bool(&obj, "available", available);
    if (available || coredump_avail) {
        bb_http_resp_json_obj_set_int(&obj, "boots_since",
                                      (int64_t)bb_diag_panic_boots_since());
    }

    if (available) {
        const char *reason_str = "unknown";
        esp_reset_reason_t reason = esp_reset_reason();
        switch (reason) {
            case ESP_RST_PANIC:    reason_str = "panic";    break;
            case ESP_RST_TASK_WDT: reason_str = "task_wdt"; break;
            case ESP_RST_INT_WDT:  reason_str = "int_wdt";  break;
            case ESP_RST_WDT:      reason_str = "wdt";      break;
            case ESP_RST_BROWNOUT: reason_str = "brownout"; break;
            default: break;
        }
        bb_http_resp_json_obj_set_str(&obj, "reset_reason", reason_str);

        char panic_buf[512];
        size_t panic_len = sizeof(panic_buf) - 1;
        if (bb_diag_panic_get(panic_buf, &panic_len) == BB_OK) {
            bb_http_resp_json_obj_set_str(&obj, "log_tail", panic_buf);
        }
    }

#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
    if (coredump_avail) {
        bb_diag_panic_summary_t summary;
        if (bb_diag_panic_coredump_get(&summary) == BB_OK) {
            bb_http_resp_json_obj_set_str(&obj, "task",      summary.task_name);
            bb_http_resp_json_obj_set_int(&obj, "exc_pc",    (int64_t)summary.exc_pc);
            bb_http_resp_json_obj_set_int(&obj, "exc_cause", (int64_t)summary.exc_cause);

            bb_http_resp_json_obj_set_arr_begin(&obj, "backtrace");
            for (uint32_t i = 0; i < summary.bt_count; i++) {
                bb_http_resp_json_obj_set_int(&obj, NULL, (int64_t)summary.bt_addrs[i]);
            }
            bb_http_resp_json_obj_set_arr_end(&obj);

            if (summary.panic_reason[0] != '\0') {
                bb_http_resp_json_obj_set_str(&obj, "panic_reason", summary.panic_reason);
            }
            if (summary.app_sha256[0] != '\0') {
                bb_http_resp_json_obj_set_str(&obj, "app_sha256", summary.app_sha256);
            }
        }
    }
#endif

    return bb_http_resp_json_obj_end(&obj);
}

// Helper: map esp_reset_reason_t to a short string
static const char *reset_reason_to_str(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:  return "poweron";
        case ESP_RST_EXT:      return "ext";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:  return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT:      return "wdt";
        case ESP_RST_DEEPSLEEP:return "deepsleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO:     return "sdio";
        default:               return "unknown";
    }
}

// GET /api/diag/boot — compact boot-anomaly summary (served from bb_cache)
static bb_err_t boot_get_handler(bb_http_request_t *req)
{
    bb_json_t obj = bb_json_obj_new();
    if (!obj) {
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t err_obj;
        bb_http_resp_json_obj_begin(req, &err_obj);
        bb_http_resp_json_obj_set_str(&err_obj, "error", "alloc failed");
        bb_http_resp_json_obj_end(&err_obj);
        return BB_ERR_NO_SPACE;
    }
    bb_cache_serialize_into(BB_DIAG_BOOT_TOPIC, obj);
    char *str = bb_json_serialize(obj);
    bb_json_free(obj);
    if (!str) {
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t err_obj;
        bb_http_resp_json_obj_begin(req, &err_obj);
        bb_http_resp_json_obj_set_str(&err_obj, "error", "serialize failed");
        bb_http_resp_json_obj_end(&err_obj);
        return BB_ERR_NO_SPACE;
    }
    bb_err_t err = bb_http_resp_set_type(req, "application/json");
    if (err == BB_OK) err = bb_http_resp_send_chunk(req, str, -1);
    if (err == BB_OK) err = bb_http_resp_send_chunk(req, NULL, 0);
    bb_json_free_str(str);
    return err;
}

// DELETE /api/diag/boot — clear panic log + abnormal-reset counter
static bb_err_t boot_delete_handler(bb_http_request_t *req)
{
    bb_diag_panic_clear();
    bb_diag_abnormal_reset_count_clear();
    bb_http_resp_set_status(req, 204);
    return bb_http_resp_send_chunk(req, NULL, 0);
}

static const bb_route_response_t s_boot_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"reset_reason\":{\"type\":\"string\"},"
      "\"wdt_resets\":{\"type\":\"integer\"},"
      "\"panic\":{\"type\":\"object\","
      "\"properties\":{"
      "\"available\":{\"type\":\"boolean\"},"
      "\"boots_since\":{\"type\":\"integer\"}},"
      "\"required\":[\"available\"]},"
      "\"pending_verify\":{\"type\":\"boolean\"},"
      "\"rolled_back\":{\"type\":\"boolean\"}},"
      "\"required\":[\"reset_reason\",\"wdt_resets\",\"panic\",\"pending_verify\",\"rolled_back\"]}",
      "current boot reset reason, WDT-reset count, panic availability, and OTA state summary" },
    { 0 },
};

static const bb_route_t s_boot_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/boot",
    .tag       = "diag",
    .summary   = "Boot anomaly summary: reset reason, abnormal-reset count, panic availability",
    .responses = s_boot_get_responses,
    .handler   = boot_get_handler,
};

static const bb_route_response_t s_boot_delete_responses[] = {
    { 204, NULL, NULL, "panic log and abnormal-reset counter cleared" },
    { 0 },
};

static const bb_route_t s_boot_delete_route = {
    .method    = BB_HTTP_DELETE,
    .path      = "/api/diag/boot",
    .tag       = "diag",
    .summary   = "Clear panic log and abnormal-reset counter",
    .responses = s_boot_delete_responses,
    .handler   = boot_delete_handler,
};

static const bb_route_response_t s_panic_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"available\":{\"type\":\"boolean\"},"
      "\"boots_since\":{\"type\":\"integer\"},"
      "\"reset_reason\":{\"type\":\"string\"},"
      "\"log_tail\":{\"type\":\"string\"},"
      "\"task\":{\"type\":\"string\"},"
      "\"exc_pc\":{\"type\":\"integer\"},"
      "\"exc_cause\":{\"type\":\"integer\"},"
      "\"backtrace\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
      "\"panic_reason\":{\"type\":\"string\"}},"
      "\"required\":[\"available\"]}",
      "panic log status, log tail, coredump backtrace, and panic reason text (when available)" },
    { 0 },
};

static const bb_route_t s_panic_get_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/diag/panic",
    .tag      = "diag",
    .summary  = "Get panic log from previous abnormal boot",
    .responses = s_panic_get_responses,
    .handler  = panic_get_handler,
};

#ifdef CONFIG_BB_DIAG_PANIC_TRIGGER
static bb_err_t panic_trigger_handler(bb_http_request_t *req)
{
    (void)req;
    volatile int *p = NULL;
    *p = 0;
    return BB_OK;
}

static const bb_route_response_t s_panic_trigger_responses[] = {
    { 500, NULL, NULL, "never returned — handler panics before sending response" },
    { 0 },
};

static const bb_route_t s_panic_trigger_route = {
    .method    = BB_HTTP_POST,
    .path      = "/api/diag/panic/trigger",
    .tag       = "diag",
    .summary   = "Force a panic via null dereference (debug builds only)",
    .responses = s_panic_trigger_responses,
    .handler   = panic_trigger_handler,
};
#endif

#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
static bb_err_t coredump_get_handler(bb_http_request_t *req)
{
    size_t size = bb_diag_panic_coredump_size();
    if (size == 0) {
        bb_http_resp_set_status(req, 404);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "no coredump available");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_NOT_FOUND;
    }

    /* Parse ?consume query param: truthy = "1" or "true". */
    char consume_val[8] = {0};
    bool consume = false;
    if (bb_http_req_query_key_value(req, "consume", consume_val, sizeof(consume_val)) == BB_OK) {
        consume = (strcmp(consume_val, "1") == 0 || strcmp(consume_val, "true") == 0);
    }

    // Allocate in chunks to keep heap pressure manageable on a panicked device.
    // 4KB chunks; one buffer for the whole file would be ~64KB which we'd rather avoid.
    enum { CHUNK = 4096 };
    uint8_t *chunk = bb_malloc_prefer_spiram(CHUNK);
    if (!chunk) {
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "alloc failed");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_NO_SPACE;
    }

    bb_http_resp_set_status(req, 200);
    bb_http_resp_set_type(req, "application/octet-stream");
    char content_len[24];
    snprintf(content_len, sizeof content_len, "%zu", size);
    bb_http_resp_set_header(req, "Content-Length", content_len);
    bb_http_resp_set_header(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");

    /* Emit X-Coredump-App-SHA256 so the caller knows which build the dump is from
     * without having to download and decode it. */
    char app_sha[BB_DIAG_PANIC_APP_SHA256_MAX];
    if (bb_diag_panic_app_sha(app_sha, sizeof(app_sha)) == BB_OK) {
        bb_http_resp_set_header(req, "X-Coredump-App-SHA256", app_sha);
    }

    // Use the partition API directly for chunked reads — no need to allocate the
    // entire coredump buffer to satisfy the "fits in max_len" precondition of
    // bb_diag_panic_coredump_read_bytes.
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) {
        bb_mem_free(chunk);
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "partition not found");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_STATE;
    }
    size_t addr = 0, total = 0;
    if (esp_core_dump_image_get(&addr, &total) != ESP_OK) {
        bb_mem_free(chunk);
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "image_get failed");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_STATE;
    }
    size_t part_offset = (addr >= part->address) ? (addr - part->address) : 0;

    size_t sent = 0;
    bb_err_t err = BB_OK;
    while (sent < total) {
        size_t want = (total - sent < CHUNK) ? (total - sent) : CHUNK;
        if (esp_partition_read(part, part_offset + sent, chunk, want) != ESP_OK) {
            err = BB_ERR_INVALID_STATE;
            break;
        }
        err = bb_http_resp_send_chunk(req, (const char *)chunk, (int)want);
        if (err != BB_OK) break;
        sent += want;
    }
    // Terminate the chunked response (NULL/0 sentinel).
    if (err == BB_OK) {
        err = bb_http_resp_send_chunk(req, NULL, 0);
    }
    bb_mem_free(chunk);

    /* Erase only after a fully successful stream — a dropped connection must not
     * silently discard the crash evidence. */
    if (err == BB_OK && consume) {
        bb_diag_panic_coredump_erase();
    }

    return err;
}

static const bb_route_param_t s_coredump_get_params[] = {
    {
        .name        = "consume",
        .in          = "query",
        .description = "Set to 1 or true to erase the coredump from flash after a fully "
                       "successful transfer. A dropped download does NOT erase the dump.",
        .required    = false,
        .schema_type = "string",
    },
};

static const bb_route_response_t s_coredump_get_responses[] = {
    { 200, "application/octet-stream", NULL,
      "raw coredump bytes; includes X-Coredump-App-SHA256 header with the crashing "
      "app ELF SHA256 hex string (identifies the build without downloading). "
      "Pass ?consume=1 to erase the coredump from flash only after the full image "
      "has been streamed successfully — a dropped download does NOT erase the dump." },
    { 404, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "no coredump available" },
    { 500, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "allocation failure, partition not found, or coredump image_get failed" },
    { 0 },
};

static const bb_route_t s_coredump_get_route = {
    .method            = BB_HTTP_GET,
    .path              = "/api/diag/coredump",
    .tag               = "diag",
    .summary           = "Download raw coredump partition bytes; ?consume=1 erases after successful transfer",
    .responses         = s_coredump_get_responses,
    .parameters        = s_coredump_get_params,
    .parameters_count  = 1,
    .handler           = coredump_get_handler,
};
#endif /* CONFIG_BB_DIAG_PANIC_COREDUMP */

// --- heap ---

// GET /api/diag/heap[?check=true]
// Optional query param ?check=true runs heap_caps_check_integrity_all and
// appends "integrity_ok": bool to the response.
static bb_err_t heap_get_handler(bb_http_request_t *req)
{
    struct cap_entry { const char *name; uint32_t caps; };
    static const struct cap_entry caps[] = {
        { "internal", MALLOC_CAP_INTERNAL },
        { "dma",      MALLOC_CAP_DMA },
        { "spiram",   MALLOC_CAP_SPIRAM },
        { "exec",     MALLOC_CAP_EXEC },
        { "default",  MALLOC_CAP_DEFAULT },
    };

    char check_val[8];
    bool run_check = (bb_http_req_query_key_value(req, "check", check_val, sizeof(check_val)) == BB_OK
                      && strcmp(check_val, "true") == 0);

    bool integrity_ok = false;
    if (run_check) {
        integrity_ok = heap_caps_check_integrity_all(true);
    }

    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;

    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        if (heap_caps_get_total_size(caps[i].caps) == 0) continue;
        multi_heap_info_t info;
        heap_caps_get_info(&info, caps[i].caps);
        bb_http_resp_json_obj_set_obj_begin(&obj, caps[i].name);
        bb_http_resp_json_obj_set_int(&obj, "free",               (int64_t)info.total_free_bytes);
        bb_http_resp_json_obj_set_int(&obj, "allocated",          (int64_t)info.total_allocated_bytes);
        bb_http_resp_json_obj_set_int(&obj, "largest_free_block", (int64_t)info.largest_free_block);
        bb_http_resp_json_obj_set_int(&obj, "minimum_ever_free",  (int64_t)info.minimum_free_bytes);
        bb_http_resp_json_obj_set_obj_end(&obj);
    }

    if (run_check) {
        bb_http_resp_json_obj_set_bool(&obj, "integrity_ok", integrity_ok);
    }

    return bb_http_resp_json_obj_end(&obj);
}

static const bb_route_param_t s_heap_get_params[] = {
    {
        .name        = "check",
        .in          = "query",
        .description = "Set to true to run heap_caps_check_integrity_all and append "
                       "integrity_ok boolean to the response.",
        .required    = false,
        .schema_type = "string",
    },
};

static const bb_route_response_t s_heap_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"integrity_ok\":{\"type\":\"boolean\"}},"
      "\"additionalProperties\":{"
      "\"type\":\"object\","
      "\"properties\":{"
      "\"free\":{\"type\":\"integer\"},"
      "\"allocated\":{\"type\":\"integer\"},"
      "\"largest_free_block\":{\"type\":\"integer\"},"
      "\"minimum_ever_free\":{\"type\":\"integer\"}}}}",
      "per-capability heap stats; optional ?check=true appends integrity_ok field" },
    { 0 },
};

static const bb_route_t s_heap_get_route = {
    .method           = BB_HTTP_GET,
    .path             = "/api/diag/heap",
    .tag              = "diag",
    .summary          = "Per-capability heap statistics; pass ?check=true to run integrity check",
    .responses        = s_heap_get_responses,
    .parameters       = s_heap_get_params,
    .parameters_count = 1,
    .handler          = heap_get_handler,
};

// --- tasks ---
// Requires CONFIG_FREERTOS_USE_TRACE_FACILITY=y (provides uxTaskGetSystemState).

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
static bb_err_t tasks_get_handler(bb_http_request_t *req)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *arr = bb_malloc_prefer_spiram(sizeof(TaskStatus_t) * n);
    if (!arr) {
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t err_obj;
        bb_http_resp_json_obj_begin(req, &err_obj);
        bb_http_resp_json_obj_set_str(&err_obj, "error", "alloc failed");
        bb_http_resp_json_obj_end(&err_obj);
        return BB_ERR_NO_SPACE;
    }

    uint32_t total_runtime = 0;
    UBaseType_t got = uxTaskGetSystemState(arr, n, &total_runtime);

    // Emit as a top-level JSON array via the obj stream (array-at-root pattern:
    // use arr_begin with a wrapping object field is not applicable here; instead
    // we use bb_http_resp_json_arr_begin directly).
    bb_http_json_stream_t stream;
    bb_err_t err = bb_http_resp_json_arr_begin(req, &stream);
    if (err != BB_OK) { bb_mem_free(arr); return err; }

    for (UBaseType_t i = 0; i < got; i++) {
        const char *state_str = "?";
        switch (arr[i].eCurrentState) {
            case eRunning:   state_str = "running";   break;
            case eReady:     state_str = "ready";     break;
            case eBlocked:   state_str = "blocked";   break;
            case eSuspended: state_str = "suspended"; break;
            case eDeleted:   state_str = "deleted";   break;
            case eInvalid:   state_str = "invalid";   break;
            default: break;
        }
        bb_json_t t = bb_json_obj_new();
        bb_json_obj_set_string(t, "name",      arr[i].pcTaskName);
        bb_json_obj_set_number(t, "prio",      (double)arr[i].uxCurrentPriority);
        bb_json_obj_set_number(t, "base_prio", (double)arr[i].uxBasePriority);
        bb_json_obj_set_number(t, "stack_hwm", (double)arr[i].usStackHighWaterMark);
        bb_json_obj_set_string(t, "state",     state_str);
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
        bb_json_obj_set_number(t, "core",    (double)arr[i].xCoreID);
#endif
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        bb_json_obj_set_number(t, "runtime", (double)arr[i].ulRunTimeCounter);
#endif
        // Additive enrichment (B1-445): cross-reference bb_task_registry by
        // name via the pure lookup fn. Omitted (not null) when the task did
        // not self-register — existing fields above are byte-unchanged.
        {
            uint32_t reg_budget = 0;
            bool     reg_wdt    = false;
            if (bb_task_registry_lookup_budget(arr[i].pcTaskName, &reg_budget, &reg_wdt)) {
                bb_json_obj_set_number(t, "stack_budget_bytes", (double)reg_budget);
                bb_json_obj_set_bool(t, "wdt_subscribed", reg_wdt);
            }
        }
        bb_http_resp_json_arr_emit(&stream, t);
        bb_json_free(t);
    }
    bb_mem_free(arr);
    return bb_http_resp_json_arr_end(&stream);
}

static const bb_route_response_t s_tasks_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"array\","
      "\"items\":{\"type\":\"object\","
      "\"properties\":{"
      "\"name\":{\"type\":\"string\"},"
      "\"prio\":{\"type\":\"integer\"},"
      "\"base_prio\":{\"type\":\"integer\"},"
      "\"stack_hwm\":{\"type\":\"integer\"},"
      "\"state\":{\"type\":\"string\"},"
      "\"core\":{\"type\":\"integer\"},"
      "\"runtime\":{\"type\":\"integer\"},"
      "\"stack_budget_bytes\":{\"type\":\"integer\"},"
      "\"wdt_subscribed\":{\"type\":\"boolean\"}}}}",
      "task list; core requires CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID; "
      "runtime requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS; "
      "stack_budget_bytes/wdt_subscribed present only for tasks self-registered "
      "via bb_task_registry" },
    { 500, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "allocation failure (malloc for TaskStatus_t array failed)" },
    { 0 },
};

static const bb_route_t s_tasks_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/tasks",
    .tag       = "diag",
    .summary   = "List all FreeRTOS tasks with state, priority, and stack high-water mark "
                 "(requires CONFIG_FREERTOS_USE_TRACE_FACILITY=y; "
                 "core field: CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID; "
                 "runtime field: CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)",
    .responses = s_tasks_get_responses,
    .handler   = tasks_get_handler,
};
#endif /* CONFIG_FREERTOS_USE_TRACE_FACILITY */

// --- sockets ---
// Walks LWIP TCP PCB lists under LOCK_TCPIP_CORE and emits a JSON snapshot
// of per-state counts and individual PCBs. Designed for hunting CLOSE_WAIT
// pile-ups that exhaust the LWIP socket pool under sustained SSE churn.

static const char *s_tcp_state_names[] = {
    "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD",
    "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2",
    "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT"
};
#define S_TCP_STATE_COUNT (sizeof(s_tcp_state_names) / sizeof(s_tcp_state_names[0]))

// Number of state names must cover all states (0..TIME_WAIT=10).
_Static_assert(S_TCP_STATE_COUNT == 11, "tcp state name table mismatch");

static bb_err_t sockets_get_handler(bb_http_request_t *req)
{
    // Walk PCB lists under core lock and snapshot counts + per-entry data.
    uint32_t by_state[S_TCP_STATE_COUNT] = {0};
    uint32_t in_use = 0;

    // PCB snapshot: at most CONFIG_LWIP_MAX_SOCKETS entries.
    typedef struct { uint16_t local_port; uint16_t remote_port;
                     char remote_ip[40]; uint32_t state_idx; } pcb_snap_t;
    pcb_snap_t *snaps = bb_malloc_prefer_spiram(sizeof(pcb_snap_t) * (size_t)CONFIG_LWIP_MAX_SOCKETS);
    size_t snap_count = 0;

    LOCK_TCPIP_CORE();
    struct tcp_pcb *lists[4];
    lists[0] = tcp_active_pcbs;
    lists[1] = tcp_tw_pcbs;
    lists[2] = tcp_bound_pcbs;
    lists[3] = (struct tcp_pcb *)tcp_listen_pcbs.pcbs;

    for (int li = 0; li < 4; li++) {
        for (struct tcp_pcb *p = lists[li]; p != NULL; p = p->next) {
            enum tcp_state st = p->state;
            uint32_t idx = (uint32_t)st;
            if (idx < S_TCP_STATE_COUNT) by_state[idx]++;
            if (st != CLOSED) in_use++;
            if (snaps && snap_count < (size_t)CONFIG_LWIP_MAX_SOCKETS) {
                pcb_snap_t *s = &snaps[snap_count++];
                s->local_port  = p->local_port;
                s->remote_port = p->remote_port;
                s->state_idx   = idx;
                ipaddr_ntoa_r(&p->remote_ip, s->remote_ip, sizeof(s->remote_ip));
            }
        }
    }
    UNLOCK_TCPIP_CORE();

    bb_log_i(TAG, "sockets: in_use=%"PRIu32" CLOSE_WAIT=%"PRIu32" TIME_WAIT=%"PRIu32,
             in_use, by_state[CLOSE_WAIT], by_state[TIME_WAIT]);

    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) { bb_mem_free(snaps); return err; }

    bb_http_resp_json_obj_set_int(&obj, "lwip_max_sockets", (int64_t)CONFIG_LWIP_MAX_SOCKETS);
    bb_http_resp_json_obj_set_int(&obj, "in_use",           (int64_t)in_use);

    bb_http_resp_json_obj_set_obj_begin(&obj, "by_state");
    for (uint32_t i = 0; i < S_TCP_STATE_COUNT; i++) {
        bb_http_resp_json_obj_set_int(&obj, s_tcp_state_names[i], (int64_t)by_state[i]);
    }
    bb_http_resp_json_obj_set_obj_end(&obj);

    bb_http_resp_json_obj_set_arr_begin(&obj, "pcbs");
    if (snaps) {
        for (size_t i = 0; i < snap_count; i++) {
            const char *state_name = (snaps[i].state_idx < S_TCP_STATE_COUNT)
                                     ? s_tcp_state_names[snaps[i].state_idx] : "UNKNOWN";
            bb_http_resp_json_obj_set_obj_begin(&obj, NULL);
            bb_http_resp_json_obj_set_int(&obj, "local_port",  (int64_t)snaps[i].local_port);
            bb_http_resp_json_obj_set_str(&obj, "remote_ip",   snaps[i].remote_ip);
            bb_http_resp_json_obj_set_int(&obj, "remote_port", (int64_t)snaps[i].remote_port);
            bb_http_resp_json_obj_set_str(&obj, "state",       state_name);
            bb_http_resp_json_obj_set_obj_end(&obj);
        }
    }
    bb_http_resp_json_obj_set_arr_end(&obj);

    bb_mem_free(snaps);
    return bb_http_resp_json_obj_end(&obj);
}

static const bb_route_response_t s_sockets_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"lwip_max_sockets\":{\"type\":\"integer\"},"
      "\"in_use\":{\"type\":\"integer\"},"
      "\"by_state\":{\"type\":\"object\",\"additionalProperties\":{\"type\":\"integer\"}},"
      "\"pcbs\":{\"type\":\"array\",\"items\":{"
      "\"type\":\"object\","
      "\"properties\":{"
      "\"local_port\":{\"type\":\"integer\"},"
      "\"remote_ip\":{\"type\":\"string\"},"
      "\"remote_port\":{\"type\":\"integer\"},"
      "\"state\":{\"type\":\"string\"}}}}}}",
      "LWIP TCP socket state distribution: per-state counts and per-PCB detail" },
    { 0 },
};

static const bb_route_t s_sockets_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/sockets",
    .tag       = "diag",
    .summary   = "LWIP TCP socket state distribution (in_use, by_state counts, per-PCB detail)",
    .responses = s_sockets_get_responses,
    .handler   = sockets_get_handler,
};

// GET /api/diag/net — network diagnostic counters (TA-505).
// Sources: bb_net_health snapshot, bb_clock_now_ms, bb_http handler counts.
extern size_t bb_http_route_handler_count(void);
extern size_t bb_http_route_handler_cap(void);

static bb_err_t diag_net_handler(bb_http_request_t *req)
{
    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;

    bb_http_resp_json_obj_set_int(&obj, "uptime_ms",           (int64_t)bb_clock_now_ms());
    bb_http_resp_json_obj_set_int(&obj, "http_handler_count",  (int64_t)bb_http_route_handler_count());
    bb_http_resp_json_obj_set_int(&obj, "http_handler_cap",    (int64_t)bb_http_route_handler_cap());

    bb_net_health_status_t snap;
    if (bb_net_health_get_status(&snap) == BB_OK) {
        bb_http_resp_json_obj_set_int(&obj, "rssi",                   (int64_t)snap.rssi);
        bb_http_resp_json_obj_set_int(&obj, "disc_age_s",             (int64_t)snap.disc_age_s);
        bb_http_resp_json_obj_set_int(&obj, "last_disconnect_reason", (int64_t)snap.last_disconnect_reason);
        bb_http_resp_json_obj_set_int(&obj, "lost_ip_recoveries",     (int64_t)snap.lost_ip_recoveries);
        bb_http_resp_json_obj_set_int(&obj, "lost_ip_age_s",          (int64_t)snap.lost_ip_age_s);
        bb_http_resp_json_obj_set_int(&obj, "egress_dead_recoveries", (int64_t)snap.egress_dead_recoveries);

        bb_http_resp_json_obj_set_obj_begin(&obj, "mqtt");
        bb_http_resp_json_obj_set_int(&obj, "reconnect_count", (int64_t)snap.mqtt_reconnect_count);
        bb_http_resp_json_obj_set_int(&obj, "disc_age_s",      (int64_t)snap.mqtt_disc_age_s);
        bb_http_resp_json_obj_set_int(&obj, "disc_reason",     (int64_t)snap.mqtt_disc_reason);
        bb_http_resp_json_obj_set_int(&obj, "tls_fail",        (int64_t)snap.mqtt_tls_fail);
        bb_http_resp_json_obj_set_obj_end(&obj);

        bb_http_resp_json_obj_set_obj_begin(&obj, "http");
        bb_http_resp_json_obj_set_int(&obj, "consec_failures", (int64_t)snap.http_consec_failures);
        bb_http_resp_json_obj_set_int(&obj, "tls_fail",        (int64_t)snap.http_tls_fail);
        bb_http_resp_json_obj_set_int(&obj, "last_status",     (int64_t)snap.http_last_status);
        bb_http_resp_json_obj_set_obj_end(&obj);
    }

    return bb_http_resp_json_obj_end(&obj);
}

static const bb_route_response_t s_diag_net_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"uptime_ms\":{\"type\":\"integer\"},"
      "\"http_handler_count\":{\"type\":\"integer\"},"
      "\"http_handler_cap\":{\"type\":\"integer\"},"
      "\"rssi\":{\"type\":\"integer\"},"
      "\"disc_age_s\":{\"type\":\"integer\"},"
      "\"last_disconnect_reason\":{\"type\":\"integer\"},"
      "\"lost_ip_recoveries\":{\"type\":\"integer\"},"
      "\"lost_ip_age_s\":{\"type\":\"integer\"},"
      "\"egress_dead_recoveries\":{\"type\":\"integer\"},"
      "\"mqtt\":{\"type\":\"object\",\"properties\":{"
      "\"reconnect_count\":{\"type\":\"integer\"},"
      "\"disc_age_s\":{\"type\":\"integer\"},"
      "\"disc_reason\":{\"type\":\"integer\"},"
      "\"tls_fail\":{\"type\":\"integer\"}}},"
      "\"http\":{\"type\":\"object\",\"properties\":{"
      "\"consec_failures\":{\"type\":\"integer\"},"
      "\"tls_fail\":{\"type\":\"integer\"},"
      "\"last_status\":{\"type\":\"integer\"}}}},"
      "\"required\":[\"uptime_ms\"]}",
      "network diagnostic counters relocated from /api/info and /api/health" },
    { 0 },
};

static const bb_route_t s_diag_net_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/net",
    .tag       = "diag",
    .summary   = "Network diagnostic counters (uptime, rssi, reconnect counts, tls failures)",
    .responses = s_diag_net_responses,
    .handler   = diag_net_handler,
};

static bb_err_t bb_diag_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t err = bb_http_register_described_route(server, &s_boot_get_route);
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, &s_boot_delete_route);
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, &s_panic_get_route);
    if (err != BB_OK) return err;

#ifdef CONFIG_BB_DIAG_PANIC_TRIGGER
    err = bb_http_register_described_route(server, &s_panic_trigger_route);
    if (err != BB_OK) return err;
    bb_log_w(TAG, "panic trigger route ENABLED — debug build only");
#endif

#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
    err = bb_http_register_described_route(server, &s_coredump_get_route);
    if (err != BB_OK) return err;
#endif

    err = bb_http_register_described_route(server, &s_heap_get_route);
    if (err != BB_OK) return err;

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    err = bb_http_register_described_route(server, &s_tasks_get_route);
    if (err != BB_OK) return err;
#endif

    err = bb_http_register_described_route(server, &s_sockets_get_route);
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, &s_diag_net_route);
    if (err != BB_OK) return err;

    /* B1-290: NVS delete route — single DELETE /api/nvs with JSON body. */
    err = bb_nv_delete_routes_init(server);
    if (err != BB_OK) return err;

    // Register diag.boot in bb_cache (owned struct, serializer shared with SSE).
    {
        bb_err_t cerr = bb_cache_register(BB_DIAG_BOOT_TOPIC, NULL,
                                          sizeof(bb_diag_boot_snap_t),
                                          bb_diag_boot_serialize);
        if (cerr != BB_OK) {
            bb_log_w(TAG, "bb_cache_register diag.boot failed: %d", (int)cerr);
        }
    }

    // Register retained diag.boot event topic and publish initial snapshot.
    {
        static const char k_diag_boot_schema[] =
            "{\"title\":\"DiagBoot\",\"x-sse-topic\":\"diag.boot\",\"type\":\"object\","
            "\"properties\":{"
            "\"reset_reason\":{\"type\":\"string\"},"
            "\"wdt_resets\":{\"type\":\"integer\"},"
            "\"panic\":{\"type\":\"object\",\"properties\":{"
            "\"available\":{\"type\":\"boolean\"},"
            "\"boots_since\":{\"type\":\"integer\"}}},"
            "\"pending_verify\":{\"type\":\"boolean\"},"
            "\"rolled_back\":{\"type\":\"boolean\"}},"
            "\"required\":[\"reset_reason\",\"wdt_resets\",\"panic\","
            "\"pending_verify\",\"rolled_back\"]}";

        bb_err_t terr = bb_event_topic_register(BB_DIAG_BOOT_TOPIC, &s_boot_topic);
        if (terr != BB_OK) {
            bb_log_w(TAG, "diag.boot topic register failed: %d", (int)terr);
        } else {
            bb_openapi_register_topic_schema(BB_DIAG_BOOT_TOPIC, k_diag_boot_schema, "DiagBoot");
#if defined(CONFIG_BB_DIAG_AUTO_ATTACH) && CONFIG_BB_DIAG_AUTO_ATTACH
            {
                bb_err_t aerr = bb_event_routes_attach_ex(BB_DIAG_BOOT_TOPIC, true);
                if (aerr != BB_OK) {
                    bb_log_w(TAG, "auto-attach failed for '" BB_DIAG_BOOT_TOPIC "': %d",
                             (int)aerr);
                }
            }
#endif
            // Publish initial retained snapshot and wire the on_validated callback
            // so the snapshot re-posts when the image self-validates.
            diag_boot_publish();
            bb_ota_validator_set_on_validated(diag_boot_publish);
        }
    }

    bb_log_i(TAG, "diag routes registered");
    return BB_OK;
}

// bb_diag routes have no ordering dependency on other components' inits, so
// register at the default order (0).
BB_INIT_REGISTER(bb_diag_routes, bb_diag_routes_init);

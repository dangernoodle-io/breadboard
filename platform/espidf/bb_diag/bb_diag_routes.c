#include "bb_diag.h"
#include "bb_http.h"
#include "bb_info.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"

#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
#include "esp_core_dump.h"
#include "esp_partition.h"
#include <stdio.h>
#endif

static const char *TAG = "bb_diag_routes";

static bb_err_t panic_get_handler(bb_http_request_t *req)
{
    bb_json_t root = bb_json_obj_new();
    if (!root) {
        return bb_http_resp_send_err(req, 500, "JSON alloc failed");
    }

    bool available = bb_diag_panic_available();
    bool coredump_avail = bb_diag_panic_coredump_available();
    bb_json_obj_set_bool(root, "available", available);
    if (available || coredump_avail) {
        bb_json_obj_set_number(root, "boots_since", (double)bb_diag_panic_boots_since());
    }

    if (available) {
        // Get reset reason string
        const char *reason_str = "unknown";
        esp_reset_reason_t reason = esp_reset_reason();
        switch (reason) {
            case ESP_RST_PANIC:      reason_str = "panic"; break;
            case ESP_RST_TASK_WDT:   reason_str = "task_wdt"; break;
            case ESP_RST_INT_WDT:    reason_str = "int_wdt"; break;
            case ESP_RST_WDT:        reason_str = "wdt"; break;
            case ESP_RST_BROWNOUT:   reason_str = "brownout"; break;
            default: break;
        }
        bb_json_obj_set_string(root, "reset_reason", reason_str);

        // Retrieve panic log
        char panic_buf[512];
        size_t panic_len = sizeof(panic_buf) - 1;
        if (bb_diag_panic_get(panic_buf, &panic_len) == BB_OK) {
            bb_json_obj_set_string(root, "log_tail", panic_buf);
        }
    }

#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
    if (coredump_avail) {
        bb_diag_panic_summary_t summary;
        if (bb_diag_panic_coredump_get(&summary) == BB_OK) {
            bb_json_obj_set_string(root, "task", summary.task_name);
            bb_json_obj_set_number(root, "exc_pc", (double)summary.exc_pc);
            bb_json_obj_set_number(root, "exc_cause", (double)summary.exc_cause);

            bb_json_t bt = bb_json_arr_new();
            for (uint32_t i = 0; i < summary.bt_count; i++) {
                bb_json_arr_append_number(bt, (double)summary.bt_addrs[i]);
            }
            bb_json_obj_set_arr(root, "backtrace", bt);

            if (summary.panic_reason[0] != '\0') {
                bb_json_obj_set_string(root, "panic_reason", summary.panic_reason);
            }
        }
    }
#endif

    bb_http_resp_set_status(req, 200);
    bb_err_t err = bb_http_resp_send_json(req, root);
    bb_json_free(root);
    return err;
}

static bb_err_t panic_delete_handler(bb_http_request_t *req)
{
    bb_diag_panic_clear();
    bb_http_resp_set_status(req, 204);
    return bb_http_resp_send(req, NULL, 0);
}

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

static const bb_route_response_t s_panic_delete_responses[] = {
    { 204, NULL, NULL, "panic log cleared" },
    { 0 },
};

static const bb_route_t s_panic_delete_route = {
    .method   = BB_HTTP_DELETE,
    .path     = "/api/diag/panic",
    .tag      = "diag",
    .summary  = "Clear panic log",
    .responses = s_panic_delete_responses,
    .handler  = panic_delete_handler,
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
        return bb_http_resp_send_err(req, 404, "no coredump available");
    }

    // Allocate in chunks to keep heap pressure manageable on a panicked device.
    // 4KB chunks; one buffer for the whole file would be ~64KB which we'd rather avoid.
    enum { CHUNK = 4096 };
    uint8_t *chunk = malloc(CHUNK);
    if (!chunk) return bb_http_resp_send_err(req, 500, "alloc failed");

    bb_http_resp_set_status(req, 200);
    bb_http_resp_set_type(req, "application/octet-stream");
    char content_len[24];
    snprintf(content_len, sizeof content_len, "%zu", size);
    bb_http_resp_set_header(req, "Content-Length", content_len);
    bb_http_resp_set_header(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");

    // Use the partition API directly for chunked reads — no need to allocate the
    // entire coredump buffer to satisfy the "fits in max_len" precondition of
    // bb_diag_panic_coredump_read_bytes.
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) {
        free(chunk);
        return bb_http_resp_send_err(req, 500, "partition not found");
    }
    size_t addr = 0, total = 0;
    if (esp_core_dump_image_get(&addr, &total) != ESP_OK) {
        free(chunk);
        return bb_http_resp_send_err(req, 500, "image_get failed");
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
    free(chunk);
    return err;
}

static const bb_route_response_t s_coredump_get_responses[] = {
    { 200, "application/octet-stream", NULL, "raw coredump bytes" },
    { 404, NULL, NULL, "no coredump available" },
    { 0 },
};

static const bb_route_t s_coredump_get_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/diag/coredump",
    .tag      = "diag",
    .summary  = "Download raw coredump partition bytes",
    .responses = s_coredump_get_responses,
    .handler  = coredump_get_handler,
};
#endif /* CONFIG_BB_DIAG_PANIC_COREDUMP */

// --- abnormal-resets ---

static bb_err_t abnormal_resets_delete_handler(bb_http_request_t *req)
{
    bb_diag_abnormal_reset_count_clear();
    bb_http_resp_set_status(req, 204);
    return bb_http_resp_send(req, NULL, 0);
}

static const bb_route_response_t s_abnormal_resets_delete_responses[] = {
    { 204, NULL, NULL, "abnormal-reset counter cleared" },
    { 0 },
};

static const bb_route_t s_abnormal_resets_delete_route = {
    .method    = BB_HTTP_DELETE,
    .path      = "/api/diag/abnormal-resets",
    .tag       = "diag",
    .summary   = "Reset the abnormal-reset counter to zero",
    .responses = s_abnormal_resets_delete_responses,
    .handler   = abnormal_resets_delete_handler,
};

// --- heap ---

static bb_err_t heap_get_handler(bb_http_request_t *req)
{
    bb_json_t root = bb_json_obj_new();
    if (!root) return bb_http_resp_send_err(req, 500, "JSON alloc failed");

    struct cap_entry { const char *name; uint32_t caps; };
    static const struct cap_entry caps[] = {
        { "internal", MALLOC_CAP_INTERNAL },
        { "dma",      MALLOC_CAP_DMA },
        { "spiram",   MALLOC_CAP_SPIRAM },
        { "exec",     MALLOC_CAP_EXEC },
        { "default",  MALLOC_CAP_DEFAULT },
    };
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        if (heap_caps_get_total_size(caps[i].caps) == 0) continue;
        multi_heap_info_t info;
        heap_caps_get_info(&info, caps[i].caps);
        bb_json_t cap = bb_json_obj_new();
        bb_json_obj_set_number(cap, "free",               (double)info.total_free_bytes);
        bb_json_obj_set_number(cap, "allocated",          (double)info.total_allocated_bytes);
        bb_json_obj_set_number(cap, "largest_free_block", (double)info.largest_free_block);
        bb_json_obj_set_number(cap, "minimum_ever_free",  (double)info.minimum_free_bytes);
        bb_json_obj_set_obj(root, caps[i].name, cap);
    }

    bb_http_resp_set_status(req, 200);
    bb_err_t err = bb_http_resp_send_json(req, root);
    bb_json_free(root);
    return err;
}

static bb_err_t heap_check_handler(bb_http_request_t *req)
{
    bool ok = heap_caps_check_integrity_all(true);
    bb_json_t root = bb_json_obj_new();
    if (!root) return bb_http_resp_send_err(req, 500, "JSON alloc failed");
    bb_json_obj_set_bool(root, "ok", ok);
    bb_http_resp_set_status(req, ok ? 200 : 500);
    bb_err_t err = bb_http_resp_send_json(req, root);
    bb_json_free(root);
    return err;
}

static const bb_route_response_t s_heap_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"additionalProperties\":{"
      "\"type\":\"object\","
      "\"properties\":{"
      "\"free\":{\"type\":\"integer\"},"
      "\"allocated\":{\"type\":\"integer\"},"
      "\"largest_free_block\":{\"type\":\"integer\"},"
      "\"minimum_ever_free\":{\"type\":\"integer\"}}}}",
      "per-capability heap stats (internal, dma, spiram, exec, default — only present caps included)" },
    { 0 },
};

static const bb_route_t s_heap_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/heap",
    .tag       = "diag",
    .summary   = "Per-capability heap statistics",
    .responses = s_heap_get_responses,
    .handler   = heap_get_handler,
};

static const bb_route_response_t s_heap_check_responses[] = {
    { 200, "application/json", "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}}}", "heap integrity OK" },
    { 500, "application/json", NULL, "heap corruption detected; details in device log" },
    { 0 },
};

static const bb_route_t s_heap_check_route = {
    .method    = BB_HTTP_POST,
    .path      = "/api/diag/heap/check",
    .tag       = "diag",
    .summary   = "Run heap integrity check across all regions",
    .responses = s_heap_check_responses,
    .handler   = heap_check_handler,
};

// --- tasks ---
// Requires CONFIG_FREERTOS_USE_TRACE_FACILITY=y (provides uxTaskGetSystemState).

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
static bb_err_t tasks_get_handler(bb_http_request_t *req)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *arr = malloc(sizeof(TaskStatus_t) * n);
    if (!arr) return bb_http_resp_send_err(req, 500, "alloc failed");

    uint32_t total_runtime = 0;
    UBaseType_t got = uxTaskGetSystemState(arr, n, &total_runtime);

    bb_json_t root = bb_json_arr_new();
    for (UBaseType_t i = 0; i < got; i++) {
        bb_json_t t = bb_json_obj_new();
        bb_json_obj_set_string(t, "name",       arr[i].pcTaskName);
        bb_json_obj_set_number(t, "prio",       (double)arr[i].uxCurrentPriority);
        bb_json_obj_set_number(t, "base_prio",  (double)arr[i].uxBasePriority);
        bb_json_obj_set_number(t, "stack_hwm",  (double)arr[i].usStackHighWaterMark);

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
        bb_json_obj_set_string(t, "state", state_str);

#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
        bb_json_obj_set_number(t, "core", (double)arr[i].xCoreID);
#endif
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        bb_json_obj_set_number(t, "runtime", (double)arr[i].ulRunTimeCounter);
#endif
        bb_json_arr_append_obj(root, t);
    }
    free(arr);

    bb_http_resp_set_status(req, 200);
    bb_err_t err = bb_http_resp_send_json(req, root);
    bb_json_free(root);
    return err;
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
      "\"runtime\":{\"type\":\"integer\"}}}}",
      "task list; core requires CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID; "
      "runtime requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS" },
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

// /api/info extender: adds an optional "panic" object only when a panic
// log or coredump is present, so clean boots see no schema change.
// Always emits "abnormal_reset_count" so operators can see the lifetime counter.
static void bb_diag_info_extender(bb_json_t root)
{
    bool avail = bb_diag_panic_available();
    bool coredump = bb_diag_panic_coredump_available();

    bb_json_obj_set_number(root, "abnormal_reset_count", (double)bb_diag_abnormal_reset_count());

    if (!avail && !coredump) return;

    bb_json_t panic = bb_json_obj_new();
    bb_json_obj_set_bool(panic, "available", avail);
    bb_json_obj_set_bool(panic, "coredump", coredump);
    bb_json_obj_set_number(panic, "boots_since", (double)bb_diag_panic_boots_since());
    bb_json_obj_set_obj(root, "panic", panic);
}

static bb_err_t bb_diag_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t err = bb_http_register_described_route(server, &s_panic_get_route);
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, &s_panic_delete_route);
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

    err = bb_http_register_described_route(server, &s_abnormal_resets_delete_route);
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, &s_heap_get_route);
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, &s_heap_check_route);
    if (err != BB_OK) return err;

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    err = bb_http_register_described_route(server, &s_tasks_get_route);
    if (err != BB_OK) return err;
#endif

    bb_info_register_extender(bb_diag_info_extender);

    bb_log_i(TAG, "diag routes + info extender registered");
    return BB_OK;
}

BB_REGISTRY_REGISTER_N(bb_diag_routes, bb_diag_routes_init, 4);

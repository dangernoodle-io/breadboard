// bb_pub_rtos — telemetry source satellite: FreeRTOS task-stack high-water-marks.
// Compiled on both host (tests) and ESP-IDF.
//
// On ESP-IDF: uses uxTaskGetSystemState() to enumerate all tasks.
//   Requires CONFIG_FREERTOS_USE_TRACE_FACILITY=y (already enabled).
//   HWM words are multiplied by sizeof(StackType_t) to convert to bytes.
//
// On host: returns deterministic stub values so tests pass without FreeRTOS.
#include "bb_pub_rtos.h"
#include "bb_pub.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_openapi.h"
#include "bb_init.h"
#include "bb_task_registry.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef CONFIG_BB_PUB_RTOS_AUTO_ATTACH
#define CONFIG_BB_PUB_RTOS_AUTO_ATTACH 0
#endif

static const char *TAG = "bb_pub_rtos";

// ---------------------------------------------------------------------------
// Named critical tasks: field name → task name candidates (NULL-terminated).
// Multiple candidates handle variant task naming across esp-idf versions.
// ---------------------------------------------------------------------------

typedef struct {
    const char *field;           // JSON field emitted
    const char *names[4];        // candidate pcTaskName values
} named_task_t;

static const named_task_t s_named[] = {
    { "stack_bb_pub",  { "bb_pub",       NULL, NULL, NULL } },
    { "stack_httpd",   { "httpd",        "httpd_dispatcher", NULL, NULL } },
    { "stack_mqtt",    { "mqtt_task",    "mqtt_client", "mqtt", NULL } },
    { "stack_ipc0",    { "ipc0",         NULL, NULL, NULL } },
    { "stack_ipc1",    { "ipc1",         NULL, NULL, NULL } },
    { "stack_main",    { "main",         "main_task", NULL, NULL } },
};
#define NAMED_COUNT ((int)(sizeof(s_named) / sizeof(s_named[0])))

// Benign system tasks: tiny, idle-heavy stacks by design (ipc0/ipc1, IDLE*,
// esp_timer, Tmr Svc). Their HWM sits low and is NOT an overflow risk, so they
// are excluded from min_free_stack (which should reflect APP tasks that can
// actually overflow). Still reported individually (e.g. stack_ipc0).
bool bb_pub_rtos_is_benign_task(const char *name)
{
    if (!name) return false;
    static const char *const benign[] = {
        "ipc0", "ipc1", "IDLE", "IDLE0", "IDLE1", "esp_timer", "Tmr Svc",
    };
    for (int i = 0; i < (int)(sizeof(benign) / sizeof(benign[0])); i++) {
        if (strcmp(name, benign[i]) == 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// bb_task_registry-driven fields (B1-445) — additive. Every task that
// self-registers via bb_task_registry (see components/bb_task_registry) gets
// its own "stack_<name>" field, on top of (not replacing) the hardcoded
// s_named[] fields above. This is how new task-creation sites gain telemetry
// coverage without hand-editing s_named[].
// ---------------------------------------------------------------------------

#define STACK_FIELD_MAX (6 + BB_TASK_REGISTRY_NAME_MAX + 1)  // "stack_" + name + '\0'

// `name` always comes from a bb_task_registry_foreach callback, which never
// passes NULL (registered names are non-NULL by construction) — no NULL
// guard needed here.
static void build_stack_field_name(char *out, size_t out_len, const char *name)
{
    snprintf(out, out_len, "stack_%s", name);
}

// Kconfig bridge: mirrors components/bb_task_registry/Kconfig's
// BB_TASK_REGISTRY_MAX so the fixed on-stack snapshot array below is sized to
// match the registry's real capacity (never truncates a live registry).
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_TASK_REGISTRY_MAX
#define BB_TASK_REGISTRY_MAX CONFIG_BB_TASK_REGISTRY_MAX
#endif
#endif
#ifndef BB_TASK_REGISTRY_MAX
#define BB_TASK_REGISTRY_MAX 24
#endif

// bb_task_registry_foreach holds the registry lock across the entire call
// (see bb_task_registry.h foreach contract) and its callback MUST NOT
// allocate. bb_json_obj_set_number allocates (cJSON), so the callbacks below
// only snapshot {name, bytes} into a fixed on-stack array; the JSON fields
// are built afterward, once the lock is released.
typedef struct {
    char     name[BB_TASK_REGISTRY_NAME_MAX];
    uint32_t bytes;
} registry_snapshot_entry_t;

static void emit_registry_snapshot(bb_json_t obj, const registry_snapshot_entry_t *snap, int count)
{
    for (int i = 0; i < count; i++) {
        char field[STACK_FIELD_MAX];
        build_stack_field_name(field, sizeof(field), snap[i].name);
        bb_json_obj_set_number(obj, field, (double)snap[i].bytes);
    }
}

// ---------------------------------------------------------------------------
// Sample function — called by bb_pub_tick_once for the "rtos" subtopic.
// ---------------------------------------------------------------------------

#if defined(ESP_PLATFORM) && defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Max tasks to snapshot; stack-allocated so keep it modest.
#define RTOS_MAX_TASKS 32

// Registry-driven scan ctx + callback (B1-445) — see the comment above
// build_stack_field_name(). File-scope (not nested) since C has no portable
// nested-function support.
typedef struct {
    TaskStatus_t              *tasks;
    UBaseType_t                count;
    registry_snapshot_entry_t  snap[BB_TASK_REGISTRY_MAX];
    int                         snap_count;
} registry_scan_ctx_t;

static void registry_field_cb(const char *name, uint32_t stack_budget_bytes,
                               bool wdt_subscribed, void *cb_ctx)
{
    (void)stack_budget_bytes;
    (void)wdt_subscribed;
    registry_scan_ctx_t *sc = (registry_scan_ctx_t *)cb_ctx;
    if (sc->snap_count >= BB_TASK_REGISTRY_MAX) return;
    for (UBaseType_t i = 0; i < sc->count; i++) {
        if (strncmp(sc->tasks[i].pcTaskName, name, configMAX_TASK_NAME_LEN) == 0) {
            uint32_t bytes = (uint32_t)sc->tasks[i].usStackHighWaterMark
                             * sizeof(StackType_t);
            registry_snapshot_entry_t *e = &sc->snap[sc->snap_count++];
            strncpy(e->name, name, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
            e->bytes = bytes;
            break;
        }
    }
}

static bool rtos_sample(bb_json_t obj, void *ctx)
{
    (void)ctx;

    TaskStatus_t tasks[RTOS_MAX_TASKS];
    UBaseType_t count = uxTaskGetSystemState(tasks, RTOS_MAX_TASKS, NULL);

    // Compute global minimum HWM and the associated task name.
    UBaseType_t min_hwm_words = UINT32_MAX;
    const char *min_name = "";
    for (UBaseType_t i = 0; i < count; i++) {
        // Skip benign system tasks (ipc/idle/timer) — their tiny HWM is by
        // design and would permanently dominate the min as a false positive.
        if (bb_pub_rtos_is_benign_task(tasks[i].pcTaskName)) continue;
        if (tasks[i].usStackHighWaterMark < min_hwm_words) {
            min_hwm_words = tasks[i].usStackHighWaterMark;
            min_name = tasks[i].pcTaskName;
        }
    }

    uint32_t min_bytes = (uint32_t)min_hwm_words * sizeof(StackType_t);
    bb_json_obj_set_number(obj, "min_free_stack",      (double)min_bytes);
    bb_json_obj_set_string(obj, "min_free_stack_task", min_name);
    bb_json_obj_set_number(obj, "task_count",          (double)count);

    // Emit per-named-task fields when the task is found.
    for (int n = 0; n < NAMED_COUNT; n++) {
        for (UBaseType_t i = 0; i < count; i++) {
            bool matched = false;
            for (int k = 0; s_named[n].names[k] != NULL; k++) {
                if (strncmp(tasks[i].pcTaskName, s_named[n].names[k],
                            configMAX_TASK_NAME_LEN) == 0) {
                    matched = true;
                    break;
                }
            }
            if (matched) {
                uint32_t bytes = (uint32_t)tasks[i].usStackHighWaterMark
                                 * sizeof(StackType_t);
                bb_json_obj_set_number(obj, s_named[n].field, (double)bytes);
                break;
            }
        }
    }

    // Additive: one "stack_<name>" field per bb_task_registry entry, driven
    // by the registry rather than a hardcoded table (B1-445) — see the
    // comment above build_stack_field_name(). Does not remove or alter any
    // field emitted above. The foreach callback only snapshots name/bytes
    // (no allocation while the registry lock is held); JSON fields are built
    // from the snapshot after the lock is released.
    registry_scan_ctx_t scan_ctx = { .tasks = tasks, .count = count, .snap_count = 0 };
    bb_task_registry_foreach(registry_field_cb, &scan_ctx);
    emit_registry_snapshot(obj, scan_ctx.snap, scan_ctx.snap_count);

    return true;
}

#else  // host stub or CONFIG_FREERTOS_USE_TRACE_FACILITY=n

// Host has no real live HWM to read, so the registry-driven fields (below)
// emit the registered stack_budget_bytes verbatim — deterministic and
// host-testable (matches the value a test seeds via
// bb_task_registry_test_seed()).
typedef struct {
    registry_snapshot_entry_t snap[BB_TASK_REGISTRY_MAX];
    int                        count;
} registry_scan_ctx_host_t;

static void registry_field_cb_host(const char *name, uint32_t stack_budget_bytes,
                                    bool wdt_subscribed, void *cb_ctx)
{
    (void)wdt_subscribed;
    registry_scan_ctx_host_t *sc = (registry_scan_ctx_host_t *)cb_ctx;
    if (sc->count >= BB_TASK_REGISTRY_MAX) return;
    registry_snapshot_entry_t *e = &sc->snap[sc->count++];
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    e->bytes = stack_budget_bytes;
}

static bool rtos_sample(bb_json_t obj, void *ctx)
{
    (void)ctx;

    // Deterministic stub values — chosen to exercise min-pick logic in tests.
    bb_json_obj_set_number(obj, "min_free_stack",      2048.0);
    bb_json_obj_set_string(obj, "min_free_stack_task", "bb_pub");
    bb_json_obj_set_number(obj, "task_count",          8.0);

    // Emit a representative subset of named fields.
    bb_json_obj_set_number(obj, "stack_bb_pub",  2048.0);
    bb_json_obj_set_number(obj, "stack_httpd",   3072.0);
    bb_json_obj_set_number(obj, "stack_mqtt",    4096.0);
    bb_json_obj_set_number(obj, "stack_ipc0",    2560.0);
    bb_json_obj_set_number(obj, "stack_ipc1",    2560.0);
    bb_json_obj_set_number(obj, "stack_main",    3584.0);

    // Additive: one "stack_<name>" field per bb_task_registry entry (B1-445).
    // Snapshot under the registry lock, emit JSON after release (see
    // registry_scan_ctx_host_t / emit_registry_snapshot above).
    registry_scan_ctx_host_t scan_ctx = { .count = 0 };
    bb_task_registry_foreach(registry_field_cb_host, &scan_ctx);
    emit_registry_snapshot(obj, scan_ctx.snap, scan_ctx.count);

    return true;
}

#endif  // ESP_PLATFORM

// ---------------------------------------------------------------------------
// Schema + Registration
// ---------------------------------------------------------------------------

// RtosTelemetry uses bb_pub_register_source (not bb_pub_register_telemetry);
// it has no SSE topic, so sse_topic=NULL.
static const char k_rtos_telemetry_schema[] =
    "{\"title\":\"RtosTelemetry\",\"type\":\"object\","
    "\"properties\":{"
    "\"min_free_stack\":{\"type\":\"number\"},"
    "\"min_free_stack_task\":{\"type\":\"string\"},"
    "\"task_count\":{\"type\":\"number\"},"
    "\"stack_bb_pub\":{\"type\":\"number\"},"
    "\"stack_httpd\":{\"type\":\"number\"},"
    "\"stack_mqtt\":{\"type\":\"number\"},"
    "\"stack_ipc0\":{\"type\":\"number\"},"
    "\"stack_ipc1\":{\"type\":\"number\"},"
    "\"stack_main\":{\"type\":\"number\"}},"
    "\"required\":[\"min_free_stack\",\"min_free_stack_task\",\"task_count\"]}";

bb_err_t bb_pub_rtos_register(void)
{
    bb_openapi_register_schema("RtosTelemetry", k_rtos_telemetry_schema, NULL);

    bb_err_t err = bb_pub_register_source("rtos", rtos_sample, NULL);
    if (err == BB_OK) {
        bb_log_i(TAG, "registered rtos source");
    } else if (err != BB_ERR_NO_SPACE) {
        bb_log_w(TAG, "register_source failed: %d", err);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Auto-attach (PRE_HTTP tier, after bb_pub's own PRE_HTTP registration)
// ---------------------------------------------------------------------------

static bb_err_t bb_pub_rtos_init(void)
{
    return bb_pub_rtos_register();
}

#if CONFIG_BB_PUB_RTOS_AUTO_ATTACH
BB_INIT_REGISTER_PRE_HTTP(bb_pub_rtos, bb_pub_rtos_init);
#endif

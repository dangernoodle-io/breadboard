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
#include "bb_registry.h"
#include <stdbool.h>
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
// Sample function — called by bb_pub_tick_once for the "rtos" subtopic.
// ---------------------------------------------------------------------------

#if defined(ESP_PLATFORM) && defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Max tasks to snapshot; stack-allocated so keep it modest.
#define RTOS_MAX_TASKS 32

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

    return true;
}

#else  // host stub or CONFIG_FREERTOS_USE_TRACE_FACILITY=n

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

    return true;
}

#endif  // ESP_PLATFORM

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bb_err_t bb_pub_rtos_register(void)
{
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
BB_REGISTRY_REGISTER_PRE_HTTP(bb_pub_rtos, bb_pub_rtos_init);
#endif

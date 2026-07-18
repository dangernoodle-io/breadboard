// ESP-IDF low-stack observer for bb_health.
//
// Task-registry unification PR3: the periodic FreeRTOS task-state scan (the
// PR1 stopgap) has moved to bb_task_registry's base-scan job
// (platform/espidf/bb_task_registry/bb_task_registry_base_scan.c), which now
// owns the single uxTaskGetSystemState poll for the whole tree. This file is
// a pure OBSERVER: it registers a low-stack transition handler with
// bb_task_registry (bb_task_registry_set_low_stack_handler) and posts the
// retained "health.stack" bb_event topic on transitions into low-stack
// state. No FreeRTOS types appear here anymore.

#include "../../../components/bb_health/bb_health_stack.h"
#include "bb_health_stack_wire.h"

#include "bb_event.h"
#include "bb_event_routes.h"
#include "bb_log.h"
#include "bb_openapi.h"
#include "bb_str.h"
#include "bb_task_registry.h"

#include <inttypes.h>

static const char *TAG = "bb_health_stack";

// ---------------------------------------------------------------------------
// Kconfig defaults (not set when compiled on host)
// ---------------------------------------------------------------------------

#ifndef CONFIG_BB_HEALTH_STACK_LOW_BYTES
#define CONFIG_BB_HEALTH_STACK_LOW_BYTES 256
#endif

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static bb_event_topic_t s_topic = NULL;

// B1-1045 PR-2 wire-primitive stash: the last-published health.stack state,
// widened to bb_health_stack_wire_t. Updated (mirrored) in BOTH publish
// sites below -- low_stack_handler's transition-into-low post AND
// bb_health_stack_monitor_init's initial low=false publish -- each still
// calling the existing bb_health_stack_build_json()+bb_event_post()
// unchanged. Read by bb_health_stack_gather().
static bb_health_stack_wire_t s_last_stack;

static void low_stack_handler(const char *name, void *handle, uint32_t free_bytes, void *ctx)
{
    (void)handle;
    (void)ctx;
    if (!s_topic) return;

    char payload[128];
    int n_written = bb_health_stack_build_json(payload, sizeof(payload), name, free_bytes, true);
    if (n_written > 0) {
        size_t sz = (size_t)n_written < sizeof(payload)
                    ? (size_t)n_written : sizeof(payload) - 1;
        bb_event_post(s_topic, 1, payload, sz);
        bb_log_w(TAG, "task '%s' stack low: %" PRIu32 " bytes free", name, free_bytes);

        bb_strlcpy(s_last_stack.task, name, sizeof(s_last_stack.task));
        s_last_stack.free_bytes = (int64_t)free_bytes;
        s_last_stack.low        = true;
    }
}

// ---------------------------------------------------------------------------
// B1-1045 PR-2 wire-primitive gather -- ESP-IDF only, not host-reproducible
// ---------------------------------------------------------------------------

bb_err_t bb_health_stack_gather(bb_health_stack_wire_t *dst)
{
    if (!dst) return BB_ERR_INVALID_ARG;
    *dst = s_last_stack;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// PRE_HTTP lifecycle entry point
// ---------------------------------------------------------------------------

// Registers the low-stack handler with bb_task_registry's periodic base
// scan. No topic/openapi/event side effects — those stay in
// bb_health_stack_monitor_init (regular tier, called from bb_health_init).
bb_err_t bb_health_stack_monitor_start(void)
{
    bb_task_registry_set_low_stack_handler(low_stack_handler,
                                            (uint32_t)CONFIG_BB_HEALTH_STACK_LOW_BYTES, NULL);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public init (called by bb_health_init after the HTTP server is up)
// ---------------------------------------------------------------------------

static const char k_health_stack_schema[] =
    "{\"title\":\"HealthStack\",\"x-sse-topic\":\"health.stack\",\"type\":\"object\","
    "\"properties\":{"
    "\"task\":{\"type\":\"string\"},"
    "\"free_bytes\":{\"type\":\"integer\"},"
    "\"low\":{\"type\":\"boolean\"}},"
    "\"required\":[\"task\",\"free_bytes\",\"low\"]}";

bb_err_t bb_health_stack_monitor_init(void)
{
    bb_err_t err = bb_event_topic_register(BB_HEALTH_STACK_TOPIC, &s_topic);
    if (err != BB_OK) {
        bb_log_w(TAG, "topic register failed: %d", (int)err);
        return err;
    }

    bb_openapi_register_topic_schema(BB_HEALTH_STACK_TOPIC, k_health_stack_schema, "HealthStack");

#if defined(CONFIG_BB_HEALTH_AUTO_ATTACH) && CONFIG_BB_HEALTH_AUTO_ATTACH
    {
        bb_err_t attach_err = bb_event_routes_attach_ex(BB_HEALTH_STACK_TOPIC, true);
        if (attach_err != BB_OK) {
            bb_log_w(TAG, "auto-attach failed for '" BB_HEALTH_STACK_TOPIC "': %d",
                     (int)attach_err);
        }
    }
#endif

    // Publish initial retained snapshot so SSE clients connecting before the
    // first low-stack transition see a sane baseline state (no low task,
    // low=false).
    {
        char payload[128];
        int n = bb_health_stack_build_json(payload, sizeof(payload), "", 0, false);
        if (n > 0) {
            size_t sz = (size_t)n < sizeof(payload)
                        ? (size_t)n : sizeof(payload) - 1;
            bb_event_post(s_topic, 0, payload, sz);

            s_last_stack.task[0]    = '\0';
            s_last_stack.free_bytes = 0;
            s_last_stack.low        = false;
        }
    }

    return BB_OK;
}

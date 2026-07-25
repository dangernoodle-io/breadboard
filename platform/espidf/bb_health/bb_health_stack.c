// ESP-IDF low-stack observer for bb_health.
//
// Task-registry unification PR3: the periodic FreeRTOS task-state scan (the
// PR1 stopgap) has moved to bb_task_registry's base-scan job
// (platform/espidf/bb_task_registry/bb_task_registry_base_scan.c), which now
// owns the single uxTaskGetSystemState poll for the whole tree. This file is
// a pure OBSERVER: it registers a low-stack transition handler with
// bb_task_registry (bb_task_registry_set_low_stack_handler) and bumps the
// "health.stack" bb_data generation on transitions into low-stack state
// (B1-1045). No FreeRTOS types appear here anymore.

#include "../../../components/bb_health/bb_health_stack.h"
#include "bb_health_stack_wire.h"

#include "bb_data.h"
#include "bb_data_http.h"
#include "bb_log.h"
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

// B1-1045 PR-2 wire-primitive stash: the last-published health.stack state,
// widened to bb_health_stack_wire_t. Updated (mirrored) in BOTH publish
// sites below -- low_stack_handler's transition-into-low path AND
// bb_health_stack_monitor_init's initial low=false publish -- each still
// calling the existing bb_health_stack_build_json() as a validity gate.
// Read by bb_health_stack_gather().
static bb_health_stack_wire_t s_last_stack;

static void low_stack_handler(const char *name, void *handle, uint32_t free_bytes, void *ctx)
{
    (void)handle;
    (void)ctx;

    char payload[128];
    int n_written = bb_health_stack_build_json(payload, sizeof(payload), name, free_bytes, true);
    if (n_written > 0) {
        bb_log_w(TAG, "task '%s' stack low: %" PRIu32 " bytes free", name, free_bytes);

        bb_strlcpy(s_last_stack.task, name, sizeof(s_last_stack.task));
        s_last_stack.free_bytes = (int64_t)free_bytes;
        s_last_stack.low        = true;

        bb_data_touch(BB_HEALTH_STACK_TOPIC);
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

// SSE topic schema for "health.stack" (B1-1220: migrated to the
// bb_data_http_describe() seam -- bb_health_stack no longer links bb_openapi
// at all, see CMakeLists.txt). key and topic are both BB_HEALTH_STACK_TOPIC.
// A composition root that wants "health.stack" back in /api/openapi.json
// must wire bb_openapi_set_topic_source_fn(bb_data_http_describe_foreach) --
// see bb_openapi.h's seam doc.
//
// Doc-only bookkeeping (feeds /api/openapi.json schema synthesis) -- a
// compose failure here must not abort bring-up: schema composition is
// documentation-only and must never take down the low-stack monitor.
// Degrade and continue -- log a warning and fall through so the initial
// snapshot publish below still happens, skipping the describe call
// entirely (never describe with an empty/poisoned schema buffer). The
// bb_data_http_describe() call itself is also non-fatal on failure: it
// logs a warning and falls through rather than aborting
// bb_health_stack_monitor_init(), since its backing table
// (BB_DATA_HTTP_MAX_DESCRIBE) is shared, first-come, no-eviction, and can
// legitimately be full by the time this producer registers.
bb_err_t bb_health_stack_monitor_init(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    bb_err_t schema_rc = bb_health_stack_ensure_schema_patched();
    if (schema_rc != BB_OK) {
        bb_log_w(TAG, "health.stack schema compose failed: %d", (int)schema_rc);
    } else {
        bb_err_t describe_rc = bb_data_http_describe(BB_HEALTH_STACK_TOPIC, BB_HEALTH_STACK_TOPIC,
                                                      "HealthStack", bb_health_stack_get_schema());
        if (describe_rc != BB_OK) {
            bb_log_w(TAG, "health.stack schema describe failed: %d", (int)describe_rc);
        }
    }
#else
    bb_err_t describe_rc = bb_data_http_describe(BB_HEALTH_STACK_TOPIC, BB_HEALTH_STACK_TOPIC,
                                                  "HealthStack", bb_health_stack_get_schema());
    if (describe_rc != BB_OK) {
        bb_log_w(TAG, "health.stack schema describe failed: %d", (int)describe_rc);
    }
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

    // Publish initial retained snapshot so a bb_data consumer reading before
    // the first low-stack transition sees a sane baseline state (no low
    // task, low=false).
    {
        char payload[128];
        int n = bb_health_stack_build_json(payload, sizeof(payload), "", 0, false);
        if (n > 0) {
            s_last_stack.task[0]    = '\0';
            s_last_stack.free_bytes = 0;
            s_last_stack.low        = false;

            bb_data_touch(BB_HEALTH_STACK_TOPIC);
        }
    }

    return BB_OK;
}

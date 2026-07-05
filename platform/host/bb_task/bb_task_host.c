// bb_task — host stub for bb_task_create(). No real FreeRTOS task exists on
// host; this fabricates a distinct non-NULL handle per call (no real
// thread) so the create() path still exercises bb_task_resolve() and
// bb_task_base_upsert() in host tests, mirroring how bb_task_registry's
// self-registration is host-testable via test_seed rather than a real
// TaskHandle_t.
#include "bb_task.h"

#include <stddef.h>

// Fake "handle" pool — distinct addresses per call, no real task/thread.
// Sized generously; bb_task_create is not expected to be called at high
// volume in host tests.
#define BB_TASK_HOST_FAKE_HANDLES_MAX 32
static int s_fake_handles[BB_TASK_HOST_FAKE_HANDLES_MAX];
static int s_fake_handle_next = 0;

bb_err_t bb_task_create(const bb_task_config_t *cfg, void **out_handle)
{
    if (out_handle) {
        *out_handle = NULL;
    }
    if (!cfg) {
        return BB_ERR_INVALID_ARG;
    }

    // Host builds have exactly one "core" -- no affinity clamp to exercise
    // beyond what bb_task_resolve()'s own host tests already cover with an
    // explicit num_cores argument.
    bb_task_resolved_t resolved;
    bb_err_t err = bb_task_resolve(cfg, 1, &resolved);
    if (err != BB_OK) {
        return err;
    }

    void *handle = &s_fake_handles[s_fake_handle_next % BB_TASK_HOST_FAKE_HANDLES_MAX];
    s_fake_handle_next++;

    // No real FreeRTOS task/StackType_t on host -- record stack_bytes
    // as-is (no sizeof(StackType_t) conversion; that only happens in the
    // espidf shell).
    bb_task_base_upsert(handle, cfg->name, resolved.stack_bytes, cfg->wdt_arm);

    if (out_handle) {
        *out_handle = handle;
    }
    return BB_OK;
}

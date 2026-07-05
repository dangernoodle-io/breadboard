#pragma once
// Private shared header: pure health.stack JSON builder + the ESP-IDF
// registration entry point. No ESP-IDF or FreeRTOS types. Included by:
//   - platform/espidf/bb_health/bb_health_stack.c (low-stack observer)
//   - components/bb_health/bb_health_stack_common.c (pure impl)
//
// Task-registry unification PR3: the periodic FreeRTOS task-state scan and
// per-task low-stack debounce table this component used to own directly
// (its own bb_timer poll, PR1 stopgap) have moved to bb_task_registry's
// base-scan job (platform/espidf/bb_task_registry/
// bb_task_registry_base_scan.c). bb_health_stack is now a pure OBSERVER:
// it registers a low-stack handler via
// bb_task_registry_set_low_stack_handler() (bb_task_registry.h) and posts
// the "health.stack" event on transition-into-low.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

#include "bb_core.h"

#define BB_HEALTH_STACK_TOPIC "health.stack"

// Write JSON payload for a health.stack event into buf[buf_sz].
// Format: {"task":"<name>","free_bytes":<n>,"low":<bool>}
// Returns the number of characters that would have been written (like snprintf),
// -1 on invalid args.
int bb_health_stack_build_json(char *buf, size_t buf_sz,
                               const char *task_name,
                               uint32_t free_bytes,
                               bool low);

// PRE_HTTP: registers bb_health_stack's low-stack handler with
// bb_task_registry (see bb_task_registry_set_low_stack_handler). No topic/
// event/openapi side effects. Implemented in
// platform/espidf/bb_health/bb_health_stack.c; self-registers at PRE_HTTP
// when CONFIG_BB_HEALTH_STACK_AUTOSTART=y.
bb_err_t bb_health_stack_monitor_start(void);

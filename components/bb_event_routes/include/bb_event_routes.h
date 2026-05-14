#pragma once
#include <stddef.h>
#include <stdint.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// bb_event_routes — surfaces bb_event topics on `GET /api/events` as a
// Server-Sent Events stream.
//
// Payload contract (load-bearing): bb_event_post bytes for any topic attached
// here MUST be valid UTF-8 JSON. The route emits payload bytes verbatim in the
// SSE `data:` field. A non-JSON payload produces a malformed SSE frame that
// EventSource will drop. If size == 0 the route emits `data: {}`.
//
// Lifecycle: attached topics persist for the life of the process. The route
// registers a bb_event_ring per topic to enable replay-on-connect; new
// subscribers receive recently-buffered events before going live.

typedef struct {
    size_t max_clients;        // 0 -> CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS
    size_t per_client_queue;   // 0 -> CONFIG_BB_EVENT_ROUTES_QUEUE_DEPTH
    size_t ring_capacity;      // 0 -> CONFIG_BB_EVENT_ROUTES_RING_CAPACITY
    size_t ring_max_entry;     // 0 -> CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY
    uint32_t heartbeat_ms;     // 0 -> CONFIG_BB_EVENT_ROUTES_HEARTBEAT_MS
} bb_event_routes_cfg_t;

// Initialize the route handler. Idempotent; second call returns BB_OK.
// cfg=NULL uses Kconfig defaults.
bb_err_t bb_event_routes_init(const bb_event_routes_cfg_t *cfg);

// Attach a topic to the /api/events stream. The topic must already be
// registered via bb_event_topic_register. Idempotent per topic.
// Returns BB_ERR_NOT_FOUND if the topic isn't registered, BB_ERR_NO_SPACE if
// the per-attached-topic slot array is full.
bb_err_t bb_event_routes_attach(const char *topic_name);

#ifdef __cplusplus
}
#endif

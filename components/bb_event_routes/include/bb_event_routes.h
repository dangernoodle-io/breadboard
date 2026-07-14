#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bb_core.h"
#include "bb_event_ring.h"
#include "bb_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to a connected SSE client.
typedef struct bb_event_routes_client bb_event_routes_client_t;

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
    size_t ring_capacity;      // 0 -> CONFIG_BB_EVENT_ROUTES_RING_CAPACITY (non-retained topics)
    size_t ring_max_entry;     // 0 -> CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY
    uint32_t heartbeat_ms;     // 0 -> CONFIG_BB_EVENT_ROUTES_HEARTBEAT_MS
} bb_event_routes_cfg_t;

// Initialize the route handler. Idempotent; second call returns BB_OK.
// cfg=NULL uses Kconfig defaults.
bb_err_t bb_event_routes_init(const bb_event_routes_cfg_t *cfg);

// bbtool:init tier=regular fn=bb_event_routes_register_routes_init server=true
bb_err_t bb_event_routes_register_routes_init(bb_http_handle_t server);

// bbtool:init tier=pre_http fn=bb_event_routes_reserve_routes
bb_err_t bb_event_routes_reserve_routes(void);

// B1-492: start the SSE task-bundle pool's idle-reclaim tick. ESP-IDF-only
// background work — self-registers at PRE_HTTP tier (the house
// worker-start pattern) so route-attach init (bb_event_routes_register_routes_init, REGULAR tier)
// stays pure httpd with no timer/task creation. Does state-init +
// timer-create + timer-arm only; the reclaim tick itself never gates SSE
// reuse (bb_pool_acquire's own reap-gate owns that) — it only tears the pool
// down to 0 bytes once fully idle (0 active clients, 0 pending corpses),
// lazily recreated on the next connect. No-op under
// CONFIG_BB_EVENT_ROUTES_POOL_STATIC=y (eager-BSS pool, nothing to reclaim).
// Declared here for documentation/API-surface visibility; ESP-IDF is the
// only platform that defines it today — Arduino/host builds do not link
// this symbol (the house worker-start pattern's platform-only shape).
#ifdef ESP_PLATFORM
// bbtool:init tier=pre_http fn=bb_event_routes_start
bb_err_t bb_event_routes_start(void);
#endif

// Attach a topic to the /api/events stream. The topic must already be
// registered via bb_event_topic_register. Idempotent per topic.
// Returns BB_ERR_NOT_FOUND if the topic isn't registered, BB_ERR_NO_SPACE if
// the per-attached-topic slot array is full.
// When `retained` is true the underlying ring is created with capacity
// CONFIG_BB_EVENT_ROUTES_RETAINED_RING_CAPACITY (default 1) rather than the
// configured ring_capacity. A retained topic only needs to replay its latest
// value on connect, so capacity-1 bounds heap use on no-PSRAM boards where
// many retained topics would otherwise exhaust internal RAM.
// Preferred form for retained topics; bb_event_routes_attach is equivalent
// to bb_event_routes_attach_ex(name, false).
bb_err_t bb_event_routes_attach_ex(const char *topic_name, bool retained);

// Extended form: same as bb_event_routes_attach_ex but allows a per-topic
// max_entry override. Pass max_entry=0 to use the global configured value
// (CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY). Use when a specific topic's payload
// is larger than the global default — e.g. update.available (~430 B worst-case).
bb_err_t bb_event_routes_attach_ex2(const char *topic_name, bool retained,
                                    size_t max_entry);

// Convenience wrapper: bb_event_routes_attach_ex(topic_name, false).
bb_err_t bb_event_routes_attach(const char *topic_name);

// Diagnostics: number of topics currently attached.
size_t bb_event_routes_topic_count(void);

// Diagnostics: name and ring handle for attached topic at index `idx`.
// Returns BB_ERR_NOT_FOUND when idx >= topic_count.
// *name is a pointer into internal storage (valid until reset_for_test or process exit).
// *ring may be NULL if the topic has no ring (shouldn't happen in normal usage).
bb_err_t bb_event_routes_topic_info(size_t idx,
                                    const char **name,
                                    bb_event_ring_t *ring);

// Diagnostics: number of client slots currently in use.
size_t bb_event_routes_active_client_count(void);

// Diagnostics (B1-492): count of SSE connect attempts fast-rejected with 503
// because the requested task-bundle slot's prior occupant had not yet been
// confirmed eSuspended (the B1-484 non-blocking reap-gate) — as opposed to a
// 503 from max_clients exhaustion. A steady non-zero rate under churn is
// expected transient reuse pressure (EventSource auto-retries); a
// permanently-stuck non-zero rate would indicate the reap-gate is never
// clearing, which the idle-reclaim tick (bb_event_routes_start()) also
// guards against by proactively draining ready corpses. ESP-IDF only —
// always 0 on Arduino/host (no reap-gate exists there).
size_t bb_event_routes_slot_reuse_deferred_count(void);

// Diagnostics (B1-561): count of SSE connect attempts fast-rejected with 503
// because sse_task_bundles_ensure() (lazy first-connect heap allocation of the
// task-bundle pool) returned BB_ERR_NO_SPACE due to transient heap pressure —
// as opposed to a 503 from genuine max_clients exhaustion or the
// slot_reuse_deferred reap-gate above. A steady non-zero rate under memory
// pressure is expected transient behavior (EventSource auto-retries); it is
// not distinguishable from those other two 503 causes by body alone (the
// body is a generic "busy" value), so this counter is the way to attribute a
// 503 spike to heap pressure specifically. ESP-IDF only — always 0 on
// Arduino/host.
size_t bb_event_routes_pool_ensure_deferred_count(void);

// Acquire a new SSE client slot and subscribe to topics.
// Like bb_event_routes_client_acquire, but accepts an optional topic_filter.
// Pass NULL to subscribe to all attached topics (same as bb_event_routes_client_acquire).
// Pass a non-NULL topic name to subscribe only to that topic.
// If topic_filter doesn't match any attached topic, the client is still acquired
// but receives only heartbeats.
bb_err_t bb_event_routes_client_acquire_ex(bb_event_routes_client_t **out,
                                           const char *topic_filter);

// Acquire a new SSE client slot and subscribe to all attached topics.
// Convenience wrapper: bb_event_routes_client_acquire_ex(out, NULL).
// Preferred form is bb_event_routes_client_acquire_ex for filtered subscriptions.
bb_err_t bb_event_routes_client_acquire(bb_event_routes_client_t **out);

#ifdef __cplusplus
}
#endif

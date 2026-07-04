#pragma once

// bb_sub — cache-only ingress routing core (B1-490).
//
// bb_sub_route(topic, payload, len) is the single entry point ingress
// adapters (e.g. bb_sub_mqtt) call for every inbound message. On first
// sight of `topic` it dynamically registers a bb_cache entry — a
// PASSTHROUGH serializer, since the payload is already serialized JSON:
// bb_sub reproduces it field-for-field via bb_json_parse + a generic
// key-copy walk rather than a per-topic struct/serializer pair — then
// stores it into the topic's owned bb_cache buffer via bb_cache_update()
// (so later pull reads, e.g. bb_cache_get_serialized / bb_cache_serialize_into
// from a REST handler, reflect the same data, not a stale/empty snapshot)
// and delivers the enveloped {"ts_ms":N,"data":{...}} shape immediately to
// any direct bb_event subscriber of the topic via bb_cache_post() — see
// bb_sub.c:207-220 for the full rationale (SSE/REST byte-identical delivery).
//
// CACHE-ONLY: bb_sub never attaches a per-topic bb_event_ring / SSE fan-out
// (that would cost heap per topic — see bb_event_routes_attach_ex). A
// consumer wanting SSE delivery for a SPECIFIC routed topic attaches it
// explicitly, using the same topic name bb_cache registered.
//
// Topic bookkeeping: bb_cache_register() copies the key it is given (see
// bb_cache.h), so a caller's transient topic buffer no longer needs to
// outlive registration. bb_sub still keeps its own small persistent-storage
// registry of "seen" topic names — it backs the cache_registered dedup flag
// (skip the redundant idempotent re-register call on every message) and
// bb_sub's own drop accounting, independent of bb_cache's internals. This
// registry is capped at BB_SUB_MAX_TOPICS (mirrors bb_cache's own
// BB_CACHE_MAX_TOPICS by default) — once full, bb_sub_route() on a NEW
// topic name is dropped (already-seen topics keep routing normally) and
// bb_sub_dropped_count() is bumped.
//
// Aggregate change notification: every successful bb_sub_route() call also
// posts to a single small bb_event topic (BB_SUB_EVENT_TOPIC) carrying the
// routed topic name as payload, so a consumer (e.g. a display task) can
// wake on ANY update without polling bb_cache or attaching a per-topic SSE
// ring. Subscribe via bb_sub_subscribe() (wraps bb_event_topic_register +
// bb_event_subscribe, so the topic exists even before the first route
// call). This uses bb_event (lightweight callback dispatch, bounded by
// bb_event's own queue_depth/max_payload config) — NOT bb_event_ring, so
// there is no replay-on-subscribe and no per-subscriber ring buffer.
//
// Queue sizing / event storm caveat: EVERY successful bb_sub_route() call
// posts to BB_SUB_EVENT_TOPIC, whose dispatcher queue is bb_event's shared
// per-topic queue_depth (CONFIG_BB_EVENT_QUEUE_DEPTH, default 16 — see
// bb_event.h `bb_event_cfg_t.queue_depth`). A firehose ingress source can
// fill this queue faster than a slow subscriber drains it; bb_event drops
// the post and the subscriber misses that wakeup (bb_sub does not retry).
// Consumers routing high-rate topics MUST size queue_depth for their worst
// observed rate, or expect missed (not just delayed) notifications. This is
// a SEPARATE counter from bb_sub_dropped_count(), which only tracks drops
// at the bb_sub_route() layer (registry/payload-size overflow) — dropped
// aggregate-event posts are NOT reflected there today. Coalescing (e.g. "at
// most one in-flight notification") would close this gap but is declined
// this pass; tracked as a follow-up.

#include "bb_core.h"
#include "bb_event.h"
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Capacity constants (Kconfig bridge — pattern from bb_cache.h / bb_clock.h)
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_CACHE_MAX_TOPICS
#define BB_SUB_MAX_TOPICS CONFIG_BB_CACHE_MAX_TOPICS
#endif
#ifdef CONFIG_BB_SUB_MAX_PAYLOAD_BYTES
#define BB_SUB_MAX_PAYLOAD_BYTES CONFIG_BB_SUB_MAX_PAYLOAD_BYTES
#endif
#endif
#ifndef BB_SUB_MAX_TOPICS
#define BB_SUB_MAX_TOPICS 32
#endif
#ifndef BB_SUB_MAX_PAYLOAD_BYTES
#define BB_SUB_MAX_PAYLOAD_BYTES 1024
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Aggregate change-notification bb_event topic name. Registered lazily on
// the first bb_sub_route() or bb_sub_subscribe() call.
#define BB_SUB_EVENT_TOPIC "bb_sub.updated"

/**
 * Route one inbound (topic, payload) message into bb_cache.
 *
 * payload must be a JSON object — bb_sub's passthrough serializer copies
 * top-level object keys. Non-object payloads are stored (still delivered as
 * the enveloped {"ts_ms":N,"data":{...}} shape via bb_cache_post() to any
 * direct bb_event subscriber of the topic) but read back as an empty object
 * through bb_cache_serialize_into() / bb_cache_get_serialized().
 *
 * @param topic    NUL-terminated topic string; does NOT need to outlive the
 *                 call (bb_sub copies it into its own seen-topics storage).
 *                 Rejected (no truncation) if it is BB_SUB_TOPIC_MAX chars
 *                 or longer — a silently truncated topic could collide with
 *                 a distinct topic sharing the same truncated prefix.
 * @param payload  Message bytes.
 * @param len      Exact payload length in bytes, as measured by the caller
 *                 (e.g. the real MQTT payload length). bb_sub does NOT
 *                 infer length via strlen(payload) — payload buffers (e.g.
 *                 from an MQTT callback) are not guaranteed to be
 *                 NUL-terminated, so doing so would risk an out-of-bounds
 *                 read. len == 0 is a valid, legitimate empty payload (e.g.
 *                 an MQTT retain-delete) and is stored as such, not
 *                 special-cased.
 *
 * @return BB_OK on success.
 *         BB_ERR_INVALID_ARG if topic or payload is NULL/empty, or topic is
 *         BB_SUB_TOPIC_MAX chars or longer.
 *         BB_ERR_NO_SPACE if payload exceeds BB_SUB_MAX_PAYLOAD_BYTES, OR
 *         this is a NEW topic and bb_sub's local seen-topics registry (or
 *         bb_cache's own registry) is full — bumps bb_sub_dropped_count()
 *         and logs a warning either way.
 *         BB_ERR_NO_MEM if the per-message snapshot buffer could not be
 *         allocated — bumps bb_sub_dropped_count() and logs a warning.
 */
bb_err_t bb_sub_route(const char *topic, const char *payload, size_t len);

/**
 * Count of bb_sub_route() calls dropped due to registry/payload-size
 * overflow since boot (or the last bb_sub_reset_for_test() call).
 */
uint32_t bb_sub_dropped_count(void);

/**
 * Subscribe to the aggregate change-notification topic (BB_SUB_EVENT_TOPIC).
 * Registers the topic first if it does not exist yet, so this is safe to
 * call before any bb_sub_route() has run. cb receives the routed topic name
 * as a NUL-terminated string in `data` (size includes the NUL).
 */
bb_err_t bb_sub_subscribe(bb_event_handler_fn cb, void *user, bb_event_sub_t *out_sub);

#ifdef BB_SUB_TESTING
/**
 * Reset bb_sub's local seen-topics registry (including each entry's
 * cache_registered flag) and drop counter (test isolation). Does NOT reset
 * bb_cache or bb_event — call bb_cache_reset_for_test() /
 * bb_event_reset_for_test() separately as needed.
 *
 * ORDERING REQUIREMENT: if the test also resets bb_cache, it MUST call
 * bb_cache_reset_for_test() BEFORE this function. bb_sub_route() skips a
 * redundant bb_cache_register() once an entry's cache_registered flag is
 * set; if bb_cache's registry is cleared out from under a still-"seen"
 * bb_sub entry (this function not yet called, or called first), the next
 * bb_sub_route() for that topic will skip re-registering it in bb_cache —
 * bb_cache_update()/bb_cache_post() then fail with BB_ERR_NOT_FOUND for a
 * topic bb_sub still believes is registered.
 */
void bb_sub_reset_for_test(void);
#endif

#ifdef __cplusplus
}
#endif

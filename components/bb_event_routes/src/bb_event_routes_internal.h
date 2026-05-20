#pragma once
// Private interface shared by bb_event_routes_common.c and the per-platform
// route handlers. Not for external consumers — kept out of include/.
#include "bb_core.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bb_event_routes_client bb_event_routes_client_t;

// Allocate a client slot. BB_ERR_NO_SPACE if max_clients reached.
// Subscribes to matching topic(s) with replay-on-connect.
// If topic_filter is NULL, subscribes to all attached topics.
// If topic_filter is non-NULL, subscribes only to topics matching that name.
bb_err_t bb_event_routes_client_acquire_ex(bb_event_routes_client_t **out,
                                           const char *topic_filter);

// Convenience wrapper: bb_event_routes_client_acquire_ex(out, NULL).
bb_err_t bb_event_routes_client_acquire(bb_event_routes_client_t **out);

// Release: unsubscribe from every topic, free buffers, mark slot free.
void     bb_event_routes_client_release(bb_event_routes_client_t *c);

// Drain one queued event into a UTF-8 SSE frame (`event:` + `data:` + `id:`).
// Returns the number of bytes written (0 if queue empty or buflen too small).
size_t   bb_event_routes_drain_frame(bb_event_routes_client_t *c, char *buf, size_t buflen);

// Accessor for the per-client event signal handle. Platform code uses this
// to wait for new events without reaching into the opaque client struct.
void *   bb_event_routes_client_event(bb_event_routes_client_t *c);

uint32_t bb_event_routes_heartbeat_ms(void);

// Platform-supplied mutex shim. The "lock" is whatever opaque handle the
// platform creates; common code only treats it as a token.
void *bb_event_routes_port_lock_create(void);
void  bb_event_routes_port_lock_destroy(void *lock);
void  bb_event_routes_port_lock(void *lock);
void  bb_event_routes_port_unlock(void *lock);
void  bb_event_routes_port_notify(void *lock);  // optional: wake the SSE task

// Per-client event signal: created at acquire, destroyed at release, signaled
// by capture_cb when a new event has been enqueued so the SSE writer task can
// drain immediately instead of polling. Wait returns true if signaled, false
// on timeout — letting the writer emit heartbeats on the same path.
void *bb_event_routes_port_event_create(void);
void  bb_event_routes_port_event_destroy(void *event);
void  bb_event_routes_port_event_signal(void *event);
bool  bb_event_routes_port_event_wait(void *event, uint32_t timeout_ms);

#ifdef BB_EVENT_ROUTES_TESTING
void     bb_event_routes_reset_for_test(void);
size_t   bb_event_routes_queued_for_test(bb_event_routes_client_t *c);
uint64_t bb_event_routes_dropped_for_test(bb_event_routes_client_t *c);
void     bb_event_routes_set_allocator(void *(*c)(size_t, size_t), void (*f)(void *));
void     bb_event_routes_reset_allocator(void);
#endif

#ifdef __cplusplus
}
#endif

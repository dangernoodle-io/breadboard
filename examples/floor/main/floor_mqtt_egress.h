#pragma once

// floor_mqtt_egress — MQTT egress consumer for bb_data_http's shared drain
// (B1-1126 PR-1 of 2). examples/floor is a hand-wired composition root, not
// a component (see CLAUDE.md's composition-only model) -- floor_app.c
// acquires a DURABLE bb_data_http client mirroring the already-attached
// "log" key and wires its send_fn/abort_fn to the functions declared here,
// and test/test_host/test_floor_mqtt_egress.c #includes floor_mqtt_egress.c
// directly (examples are not part of the native scaffold's component graph,
// so this is the only way to reach it from a host test) -- one code path,
// no mirror.
//
// Reuses the existing bb_mqtt_client component (already composed in
// floor_app.c via bb_mqtt_client_init_default(), configured from NVS
// namespace "bb_mqtt") -- this file never creates a second esp-mqtt
// instance. bb_mqtt_client_default() is resolved FRESH on every send_fn
// call, never cached (bb_mqtt_client.h's CALLER CONTRACT): it legitimately
// returns NULL while MQTT is disabled or suspended.
//
// This PR deliberately does NOT bump CONFIG_BB_DATA_HTTP_MAX_CLIENTS (2):
// doing so (3) trips bb_data_http_espidf.c's own compile-time guard --
// MAX_CLIENTS*BB_DATA_HTTP_SEND_TIMEOUT_MS must stay under BB_DATA_HTTP_
// SWEEP_INTERVAL_MS, and 3*20=60 >= the 50ms default sweep interval --
// confirmed by an actual failed build, not just arithmetic. Raising
// MAX_CLIENTS without first widening that headroom (a SWEEP_INTERVAL_MS
// bump, a SEND_TIMEOUT_MS cut, or both) is therefore a SEPARATE, deliberate
// follow-up, not silently folded in here. Until it lands, floor has only 2
// bb_data_http client slots for SSE+WS+this durable MQTT consumer combined
// -- this PR's own gate (floor_mqtt_egress_should_acquire()) at least keeps
// a board with MQTT disabled from wasting one of those 2 slots on a client
// that could never deliver anything.
//
// PR2 (separate, hardware-gated, B1-1126) validates this against the real
// broker at 172.16.1.5:1883; this PR is proven entirely by host tests
// driving bb_mqtt_client's host stub.

#include "bb_core.h"
#include "bb_data_http.h"
#include "bb_mqtt_client.h"

#include <stdbool.h>

// Pure translation: bb_mqtt_client_enqueue()'s return code -> bb_data_http_
// send_fn's two-class retry contract (bb_data_http_send_fn's doc,
// bb_data_http.h). Host-testable in isolation -- callable with no MQTT or
// bb_data_http involvement at all -- the load-bearing unit this consumer's
// correctness depends on:
//
//   BB_OK                 -> BB_OK          accepted into the outbox.
//   BB_ERR_NO_SPACE        -> BB_ERR_TIMEOUT  outbox full, nothing
//                             transmitted -- genuinely retriable. Passing
//                             BB_ERR_NO_SPACE through unchanged would be
//                             misclassified FATAL by bb_data_http's two-class
//                             contract (only BB_ERR_TIMEOUT is retriable).
//   BB_ERR_INVALID_STATE   -> BB_ERR_TIMEOUT  no client right now (handle
//                             destroyed/absent) -- not permanent;
//                             bb_mqtt_client_default() may resolve non-NULL
//                             again on a future sweep.
//   anything else (including BB_ERR_VALIDATION) -> returned unchanged,
//                             i.e. FATAL to the caller -- see
//                             floor_mqtt_egress.c's KNOWN OPEN RISK note on
//                             what that means for a DURABLE client.
bb_err_t floor_mqtt_send_rc_to_bb(bb_err_t enqueue_rc);

// bb_data_http_send_fn implementation (bb_data_http_send_fn's own doc,
// bb_data_http.h, defines the full return contract this must honor).
// Resolves bb_mqtt_client_default() fresh on every call, builds
// "<FLOOR_MQTT_TOPIC_PREFIX>/<key>", and hands off via
// bb_mqtt_client_enqueue() -- never bb_mqtt_client_publish() (see
// floor_mqtt_egress.c for why). `client`/`ctx` are unused: this consumer
// carries no per-client transport state.
bb_err_t floor_mqtt_egress_send_fn(const char *key, const bb_data_http_client_t *client,
                                    const void *bytes, size_t len, void *ctx);

// bb_data_http_abort_fn implementation (bb_data_http_abort_fn's own doc,
// bb_data_http.h). A DURABLE client's abort_fn is never resolved/invoked on
// a fatal send (bb_data_http_client_cfg_t's lifetime doc) -- this exists
// only for the deliberate bb_data_http_client_request_release() path and
// installs the same graceful "release the client" behavior the core would
// otherwise fall back to with no abort_fn installed at all, made explicit
// rather than left implicit.
void floor_mqtt_egress_abort_fn(bb_data_http_client_t *client, void *ctx);

// Pure decision (review fix, mirrors floor_prov_reboot.h's
// floor_should_trigger_prov_reboot() precedent): should floor_app.c acquire
// the durable MQTT egress client this boot? bb_mqtt_client_init_default()
// returns BB_OK even when NVS "bb_mqtt".enabled != 1 (a no-op -- no client
// created), so acquiring unconditionally would permanently claim one of
// bb_data_http's compile-time client slots for a client that can only ever
// return BB_ERR_TIMEOUT from send_fn and never deliver anything, on any
// board with MQTT off. Composition-time decision only: `mqtt_handle` is the
// result of resolving bb_mqtt_client_default() once, right before the
// acquire call -- this function does not itself call bb_mqtt_client_default()
// (no I/O, no globals), so it is trivially host-testable with a fake handle
// value.
bool floor_mqtt_egress_should_acquire(bb_mqtt_client_t mqtt_handle);

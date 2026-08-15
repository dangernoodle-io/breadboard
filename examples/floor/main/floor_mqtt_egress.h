#pragma once

// floor_mqtt_egress — MQTT egress consumer for bb_data_http's shared drain
// (B1-1126 PR-1 of 2).
//
// NOT CURRENTLY COMPOSED (B1-1483): floor_app.c no longer acquires a
// bb_data_http client wired to these functions. Hardware validation of
// PR1 (#1263) found that mirroring the "log" key to MQTT is
// self-amplifying against a bounded outbox: when esp-mqtt's outbox fills,
// bb_mqtt_client logs a WARNING ("enqueue rejected: outbox full") -- that
// warning is itself a log line, so it re-enters the mirrored stream, is
// rejected, logs again. Measured on hardware: writer_dropped 1874/s and
// event_dropped 504/s sustained with MQTT enabled, vs 0.17/s and 0/s with
// it disabled (same board, same firmware, only the NVS enabled flag
// differing). USER DECISION: logs are diagnostic exhaust -- unbounded,
// human-oriented, already served by SSE and by bb_log's UDP sink -- not an
// MQTT payload. MQTT carries device state and domain events, not logs.
//
// This module is KEPT, uncomposed, as the reusable seam for when a genuine
// domain-event key needs MQTT egress: floor_mqtt_send_rc_to_bb(),
// floor_mqtt_egress_send_fn(), floor_mqtt_egress_abort_fn(), and
// floor_mqtt_egress_should_acquire() are correct, host-tested, and
// mutation-verified against bb_data_http's send_fn/abort_fn contracts --
// they are not specific to "log", only the composition (the "log"
// topic_filter + the acquire call site) has been removed from
// floor_app.c.
//
// CRITICAL PRECONDITION for whoever wires the next key onto this seam:
// bb_mqtt_client's enqueue-rejection warning MUST be rate-limited (or
// otherwise prevented from reaching the mirrored stream) BEFORE that key
// is composed. The storm above is structurally impossible today only
// because nothing is mirrored -- attaching any key without first fixing
// that warning reproduces it. Do not skip this.
//
// examples/floor is a hand-wired composition root, not a component (see
// CLAUDE.md's composition-only model) -- when composed, floor_app.c
// acquires a DURABLE bb_data_http client and wires its send_fn/abort_fn to
// the functions declared here, and test/test_host/test_floor_mqtt_egress.c
// #includes floor_mqtt_egress.c directly (examples are not part of the
// native scaffold's component graph, so this is the only way to reach it
// from a host test) -- one code path, no mirror.
//
// Reuses the existing bb_mqtt_client component (already composed in
// floor_app.c via bb_mqtt_client_init_default(), configured from NVS
// namespace "bb_mqtt") -- this file never creates a second esp-mqtt
// instance. bb_mqtt_client_default() is resolved FRESH on every send_fn
// call, never cached (bb_mqtt_client.h's CALLER CONTRACT): it legitimately
// returns NULL while MQTT is disabled or suspended.
//
// B1-1482 (later PR): CONFIG_BB_DATA_HTTP_MAX_CLIENTS's default is now 3 and
// the compile-time guard (bb_data_http_espidf.c) is blocking-aware --
// CONFIG_BB_DATA_HTTP_MAX_BLOCKING_CLIENTS*BB_DATA_HTTP_SEND_TIMEOUT_MS must
// stay under BB_DATA_HTTP_SWEEP_INTERVAL_MS, not MAX_CLIENTS*SEND_TIMEOUT_MS
// -- and floor's sdkconfig.defaults pins CONFIG_BB_DATA_HTTP_MAX_BLOCKING_
// CLIENTS=2 (SSE+WS, both genuinely blocking), leaving the 3rd MAX_CLIENTS
// slot free for exactly this consumer, PROVIDED whoever composes it sets
// `.non_blocking = true` on its bb_data_http_client_cfg_t (this client's
// send_fn, floor_mqtt_egress_send_fn(), wraps bb_mqtt_client_enqueue(),
// which never blocks -- see this file's own doc above for why publish()
// is never used instead). Omitting `.non_blocking` would count this client
// against the 2-client blocking budget instead and, on a board where SSE+WS
// are both already connected, fail bb_data_http_client_acquire()
// (BB_ERR_NO_SPACE) for this one. floor_mqtt_egress_should_acquire() still
// gates a board with MQTT disabled from wasting the 3rd slot on a client
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

// bb_mqtt_client — per-instance health internals (B1-1040). Not part of the
// public API; included via PRIV_INCLUDE_DIRS "src" from the host and
// ESP-IDF backends only (platform/{host,espidf}/bb_mqtt_client/*.c).
//
// The health snapshot descriptor and last_ok_ms clock source live here,
// shared by both backends, so the reporting/clock policy is defined in
// exactly one place (mirrors bb_tcp_client_priv.h's B1-1039 pattern).
#pragma once

#include "bb_mqtt_client.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * last_ok_ms clock source. bb_mqtt_client is a networking client (not a
 * pure primitive), so it may read a clock directly -- bb_clock_now_ms64()
 * is the canonical source (see bb_clock.h). BB_MQTT_CLIENT_TESTING builds
 * (native host tests only) get a settable mock instead, keeping
 * last_ok_ms assertions deterministic.
 */
int64_t bb_mqtt_client_priv_now_ms64(void);

/**
 * Clears *connected for a caller-initiated clean close (see bb_mqtt_client.h's
 * reporting policy) -- leaves fail_count/last_ok_ms/tls_error_code untouched,
 * since a deliberate teardown is not a transport failure. The SINGLE source
 * of the clean-close health mutation, called by both backends'
 * bb_mqtt_client_destroy() (mirrors bb_tcp_client_priv_health_close()).
 *
 * Unlike bb_tcp_client's version, this helper takes no lock itself: the
 * connected field lives directly on each backend's own handle struct (no
 * shared health sub-struct with its own lock), so callers already hold
 * whatever lock guards that handle (h->lock on espidf; no lock needed on
 * host's single-writer-per-instance convention) around this call.
 */
void bb_mqtt_client_priv_health_close(bool *connected);

#ifdef __cplusplus
}
#endif

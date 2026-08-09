// floor_mqtt_egress — see floor_mqtt_egress.h for the full contract.

#include "floor_mqtt_egress.h"
#include "bb_mqtt_client.h"
#include "bb_log.h"

#include <stdio.h>

static const char *TAG = "floor_mqtt_egress";

// Net-new topic convention: "<prefix>/<key>". The prefix is floor's own
// compile-time app identity, mirroring how examples/floor/board/*.h already
// names this build (BOARD_NAME) rather than inventing an NVS/bb_config
// field for it -- broker URI/creds already come from the existing "bb_mqtt"
// NVS namespace (bb_mqtt_client_init_default()) and must not be duplicated
// here.
#define FLOOR_MQTT_TOPIC_PREFIX "floor"

// sizeof(FLOOR_MQTT_TOPIC_PREFIX) already counts its own NUL; +1 for the
// "/" separator, + BB_DATA_HTTP_KEY_MAX (already includes ITS NUL) for the
// key -- one byte of slack over the exact bound, never truncates for any
// key bb_data_http_attach() itself accepts.
//
// Provably unreachable through the real dispatch path, not defense-in-depth
// theater: FLOOR_MQTT_EGRESS_TOPIC_MAX = sizeof("floor")+1+BB_DATA_HTTP_KEY_MAX
// = 6+1+48 = 55, and bb_data_http_attach() rejects any key >=
// BB_DATA_HTTP_KEY_MAX (48) at attach time, so the longest legal "key"
// floor_mqtt_egress_send_fn() can ever be called with is 47 chars (48-byte
// buffer including NUL) -- the longest legal rendered topic is 54 bytes,
// still one under this 55-byte buffer. The truncation guard below is kept
// anyway (a direct-call test exercises it with an oversized key that could
// never reach here through bb_data_http_attach()) as a tripwire: if either
// constant above changes and this 55 vs 48-cap arithmetic no longer holds,
// the guard -- not a silent truncation -- is what catches it.
#define FLOOR_MQTT_EGRESS_TOPIC_MAX (sizeof(FLOOR_MQTT_TOPIC_PREFIX) + 1 + BB_DATA_HTTP_KEY_MAX)

bb_err_t floor_mqtt_send_rc_to_bb(bb_err_t enqueue_rc)
{
    switch (enqueue_rc) {
    case BB_OK:
        return BB_OK;
    case BB_ERR_NO_SPACE:
        // Outbox full, nothing transmitted -- genuinely retriable. Passing
        // this through unchanged would be misclassified FATAL by
        // bb_data_http's two-class contract (bb_data_http_send_fn's doc).
        return BB_ERR_TIMEOUT;
    case BB_ERR_INVALID_STATE:
        // Handle destroyed/absent right now -- not permanent; a future
        // sweep resolves bb_mqtt_client_default() fresh and may see a live
        // client again.
        return BB_ERR_TIMEOUT;
    default:
        // BB_ERR_VALIDATION (esp-mqtt's generic reject) and anything else:
        // FATAL per bb_data_http_send_fn's two-class contract.
        //
        // KNOWN OPEN RISK (tracked separately, not fixed here): a DURABLE
        // client does not pop a frame that fails FATALLY (see
        // bb_data_http_client_cfg_t's lifetime doc, bb_data_http.h) -- a
        // persistently-rejecting frame therefore stalls this client's
        // queue forever at its head, with no CONFIG_BB_DATA_HTTP_
        // SEND_FAIL_MAX-equivalent bound on this path.
        return enqueue_rc;
    }
}

bb_err_t floor_mqtt_egress_send_fn(const char *key, const bb_data_http_client_t *client,
                                    const void *bytes, size_t len, void *ctx)
{
    (void)client;
    (void)ctx;

    // Resolved FRESH every call, never cached (bb_mqtt_client.h's CALLER
    // CONTRACT): legitimately NULL while MQTT is disabled or suspended.
    bb_mqtt_client_t h = bb_mqtt_client_default();
    if (h == NULL) {
        return BB_ERR_TIMEOUT;  // no client right now, not permanent
    }

    char topic[FLOOR_MQTT_EGRESS_TOPIC_MAX];
    int  n = snprintf(topic, sizeof(topic), FLOOR_MQTT_TOPIC_PREFIX "/%s", key);
    if (n < 0 || (size_t)n >= sizeof(topic)) {
        bb_log_w(TAG, "topic build failed/truncated for key %s", key);
        return BB_ERR_VALIDATION;  // programmer error, not transient -- fatal
    }

    // NEVER bb_mqtt_client_publish() here: it wraps a BLOCKING
    // esp_mqtt_client_publish() with no timeout, and this send_fn runs on
    // bb_data_http's SINGLE shared broadcaster task -- blocking here would
    // stall the flush for every other client (SSE/WS) sharing that task.
    // bb_mqtt_client_enqueue() is the non-blocking accept/reject seam; do
    // not "simplify" this back to publish().
    bb_err_t rc = bb_mqtt_client_enqueue(h, topic, bytes, (int)len, 0, false);
    return floor_mqtt_send_rc_to_bb(rc);
}

void floor_mqtt_egress_abort_fn(bb_data_http_client_t *client, void *ctx)
{
    (void)ctx;
    // A DURABLE client's abort_fn is never resolved/invoked on a fatal
    // send (bb_data_http_client_cfg_t's lifetime doc) -- and the deliberate
    // bb_data_http_client_request_release() teardown path reaps via a
    // direct bb_data_http_client_release() call of its own
    // (bb_data_http_common.c), never through this seam either. This
    // implementation is therefore not reachable from any call site this
    // client's DURABLE cfg exercises today; it is still installed (rather
    // than left NULL) so a future lifetime change on this client's cfg
    // degrades to the same graceful release the core's own missing-
    // abort_fn fallback already provides, made explicit rather than left
    // implicit.
    bb_data_http_client_release(client);
}

bool floor_mqtt_egress_should_acquire(bb_mqtt_client_t mqtt_handle)
{
    return mqtt_handle != NULL;
}

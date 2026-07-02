// bb_mqtt host stub — in-memory implementation for unit testing.
//
// bb_mqtt_init validates cfg and allocates a handle.
// bb_mqtt_publish records the last (and all) publish calls in a ring.
// bb_mqtt_is_connected returns a flag settable via bb_mqtt_host_set_connected.
// All operations return BB_OK; no real network IO occurs.
#include "bb_mqtt.h"
#include "bb_nv.h"
#include "bb_mqtt_reassemble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BB_MQTT_HOST_PUB_CAP 32

// Host stub has no CONFIG_BB_MQTT_RX_BUFFER_BYTES bridge (espidf-only
// Kconfig); mirror the Kconfig default so host-injected fragments behave
// the same as the on-device buffer sizing.
#define BB_MQTT_HOST_RX_BUFFER_BYTES 1024

#ifdef BB_MQTT_TESTING
static void *(*s_calloc_fn)(size_t, size_t) = NULL;
static void *bb_mqtt_calloc(size_t n, size_t sz)
{
    return s_calloc_fn ? s_calloc_fn(n, sz) : calloc(n, sz);
}
#else
#define bb_mqtt_calloc calloc
#endif

typedef struct {
    bb_mqtt_host_pub_t pubs[BB_MQTT_HOST_PUB_CAP];
    int                count;
    bool               connected;
    bool               tls;             // captured from cfg.tls at init time
    bool               ever_connected;  // set when connected goes true for first time
    uint32_t           reconnect_count;
    bb_mqtt_disc_t     disc_reason;
    bb_tls_fail_t      tls_fail;
    int                tls_error_code;
    bb_mqtt_msg_cb        msg_cb;    // B1-487: per-handle receive callback
    void                 *msg_ctx;
    bb_mqtt_reasm_state_t reasm;     // per-handle reassembly state (mirrors espidf)
#ifdef BB_MQTT_TESTING
    bool               test_subscribe_fail;  // forces bb_mqtt_subscribe to fail once set
#endif
} bb_mqtt_host_handle_t;

bb_err_t bb_mqtt_init(const bb_mqtt_cfg_t *cfg, bb_mqtt_t *out)
{
    if (!cfg || !out) return BB_ERR_INVALID_ARG;

    bb_mqtt_host_handle_t *h = bb_mqtt_calloc(1, sizeof(*h));
    if (!h) return BB_ERR_NO_SPACE;

    // Simulate "connected" by default so publish tests don't need to set flag.
    h->connected = true;
    h->tls       = cfg->tls;

    *out = h;
    return BB_OK;
}

bool bb_mqtt_is_tls(bb_mqtt_t handle)
{
    if (!handle) return false;
    return ((bb_mqtt_host_handle_t *)handle)->tls;
}

bb_err_t bb_mqtt_publish(bb_mqtt_t handle, const char *topic,
                          const char *payload, int len, int qos, bool retain)
{
    if (!handle || !topic) return BB_ERR_INVALID_ARG;
    bb_mqtt_host_handle_t *h = (bb_mqtt_host_handle_t *)handle;

    if (h->count >= BB_MQTT_HOST_PUB_CAP) {
        // Shift out oldest entry to make room.
        memmove(&h->pubs[0], &h->pubs[1],
                sizeof(h->pubs[0]) * (BB_MQTT_HOST_PUB_CAP - 1));
        h->count = BB_MQTT_HOST_PUB_CAP - 1;
    }

    bb_mqtt_host_pub_t *p = &h->pubs[h->count++];
    memset(p, 0, sizeof(*p));
    strncpy(p->topic, topic, sizeof(p->topic) - 1);
    if (payload) {
        int n = (len < 0) ? (int)strlen(payload) : len;
        if (n >= (int)sizeof(p->payload)) n = (int)sizeof(p->payload) - 1;
        memcpy(p->payload, payload, (size_t)n);
        p->payload[n] = '\0';
    }
    p->qos    = qos;
    p->retain = retain;

    return BB_OK;
}

bb_err_t bb_mqtt_subscribe(bb_mqtt_t handle, const char *topic, int qos)
{
    if (!handle || !topic) return BB_ERR_INVALID_ARG;
    (void)qos;
#ifdef BB_MQTT_TESTING
    bb_mqtt_host_handle_t *h = (bb_mqtt_host_handle_t *)handle;
    if (h->test_subscribe_fail) return BB_ERR_INVALID_STATE;
#endif
    return BB_OK;
}

// ---------------------------------------------------------------------------
// bb_mqtt_on_message / host inject (B1-487) — per-handle callback + reassembly
// state, mirroring the espidf backend (HIGH-2: no process-wide shared state).
// ---------------------------------------------------------------------------

bb_err_t bb_mqtt_on_message(bb_mqtt_t handle, bb_mqtt_msg_cb cb, void *ctx)
{
    if (!handle) return BB_ERR_INVALID_ARG;
    bb_mqtt_host_handle_t *h = (bb_mqtt_host_handle_t *)handle;

    if (cb && !h->reasm.buf) {
        h->reasm.buf = bb_mqtt_calloc(1, BB_MQTT_HOST_RX_BUFFER_BYTES);
        if (!h->reasm.buf) return BB_ERR_NO_SPACE;
        h->reasm.buf_cap = BB_MQTT_HOST_RX_BUFFER_BYTES;
        bb_mqtt_reasm_reset(&h->reasm);
    }
    h->msg_cb  = cb;
    h->msg_ctx = ctx;
    return BB_OK;
}

bool bb_mqtt_is_connected(bb_mqtt_t handle)
{
    if (!handle) return false;
    return ((bb_mqtt_host_handle_t *)handle)->connected;
}

bb_err_t bb_mqtt_get_stats(bb_mqtt_t handle, bb_mqtt_stats_t *out)
{
    if (!handle || !out) return BB_ERR_INVALID_ARG;
    bb_mqtt_host_handle_t *h = (bb_mqtt_host_handle_t *)handle;
    out->reconnect_count = h->reconnect_count;
    out->connected       = h->connected;
    out->disc_reason     = h->disc_reason;
    out->tls_fail        = h->tls_fail;
    out->tls_error_code  = h->tls_error_code;
    return BB_OK;
}

bb_err_t bb_mqtt_destroy(bb_mqtt_t handle)
{
    if (!handle) return BB_OK;
    bb_mqtt_host_handle_t *h = (bb_mqtt_host_handle_t *)handle;
    free(h->reasm.buf);
    free(handle);
    return BB_OK;
}

bb_err_t bb_mqtt_stop(bb_mqtt_t *handle_p)
{
    if (!handle_p || !*handle_p) return BB_OK;
    bb_err_t rc = bb_mqtt_destroy(*handle_p);
    *handle_p = NULL;
    return rc;
}

// ---------------------------------------------------------------------------
// Default handle (host: settable via test hook; NULL by default)
// ---------------------------------------------------------------------------

static bb_mqtt_t s_default_handle = NULL;

bb_mqtt_t bb_mqtt_default(void)
{
    return s_default_handle;
}

// ---------------------------------------------------------------------------
// bb_mqtt_stop_default — host stub
//
// Stops and NULLs the default handle (mirrors ESP-IDF behavior).
// ---------------------------------------------------------------------------

bb_err_t bb_mqtt_stop_default(void)
{
    return bb_mqtt_stop(&s_default_handle);
}

// ---------------------------------------------------------------------------
// bb_mqtt_suspend_default / bb_mqtt_resume_default — host stubs
//
// Models BOTH suspend paths:
//
//   Default (stop-only=false):
//     - suspend: DESTROYS the handle (frees, NULLs s_default_handle).
//     - resume:  RECREATES a fresh handle (connected=true, pub ring cleared).
//
//   Stop-only (stop-only=true, set via bb_mqtt_host_set_stop_only under
//   BB_MQTT_TESTING):
//     - suspend: marks the handle as "stopped" (connected=false) but does NOT
//       free it; s_default_handle remains NON-NULL.
//     - resume:  marks the handle as "started" again (connected=true); same
//       handle pointer, no recreate.
//
// bb_mqtt_host_is_suspended_default() reports s_suspended for test assertions.
// ---------------------------------------------------------------------------

static bool s_suspended  = false;
static bool s_stop_only  = false;   // controlled by bb_mqtt_host_set_stop_only

// NVS key constants (host: mirrored from espidf backend). BB_MQTT_NVS_NS is
// the SSOT namespace constant from bb_mqtt.h.
#define BB_MQTT_URI_MAX 128

bb_err_t bb_mqtt_suspend_default(void)
{
    if (s_suspended) return BB_OK;   // idempotent

    if (s_stop_only) {
        // Stop-only: keep handle resident, just mark it stopped/disconnected.
        if (s_default_handle) {
            bb_mqtt_host_handle_t *h = (bb_mqtt_host_handle_t *)s_default_handle;
            h->connected = false;
        }
        // s_default_handle intentionally left NON-NULL.
    } else {
        // Full release: destroy the handle.
        if (s_default_handle) {
            bb_mqtt_destroy(s_default_handle);
            s_default_handle = NULL;
        }
    }

    s_suspended = true;
    return BB_OK;
}

bb_err_t bb_mqtt_resume_default(void)
{
    if (!s_suspended) return BB_OK;   // idempotent

    if (s_stop_only) {
        // Stop-only: restart the resident handle (connected=true, same pointer).
        if (s_default_handle) {
            bb_mqtt_host_handle_t *h = (bb_mqtt_host_handle_t *)s_default_handle;
            h->connected = true;
        }
    } else {
        // Full release: recreate a fresh handle from NVS (or fallback uri).
        char uri[BB_MQTT_URI_MAX] = {0};
        bb_nv_get_str(BB_MQTT_NVS_NS, "uri", uri, sizeof(uri), "");

        if (!uri[0]) {
            strncpy(uri, "mqtt://localhost:1883", sizeof(uri) - 1);
        }

        bb_mqtt_cfg_t cfg = {
            .uri = uri,
            .tls = false,
        };
        bb_err_t rc = bb_mqtt_init(&cfg, &s_default_handle);
        if (rc != BB_OK) {
            // Do NOT clear s_suspended — let caller retry.
            return rc;
        }
    }

    s_suspended = false;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Host test hooks
// ---------------------------------------------------------------------------

#ifdef BB_MQTT_TESTING

const bb_mqtt_host_pub_t *bb_mqtt_host_last_pub(bb_mqtt_t handle)
{
    if (!handle) return NULL;
    bb_mqtt_host_handle_t *h = (bb_mqtt_host_handle_t *)handle;
    if (h->count == 0) return NULL;
    return &h->pubs[h->count - 1];
}

int bb_mqtt_host_pub_count(bb_mqtt_t handle)
{
    if (!handle) return 0;
    return ((bb_mqtt_host_handle_t *)handle)->count;
}

void bb_mqtt_host_set_connected(bb_mqtt_t handle, bool connected)
{
    if (!handle) return;
    ((bb_mqtt_host_handle_t *)handle)->connected = connected;
}

void bb_mqtt_host_reset(bb_mqtt_t handle)
{
    if (!handle) return;
    bb_mqtt_host_handle_t *h = (bb_mqtt_host_handle_t *)handle;
    h->count                 = 0;
    h->connected             = true;
    h->ever_connected        = false;
    h->reconnect_count       = 0;
    h->disc_reason           = BB_MQTT_DISC_NONE;
    h->tls_fail              = BB_TLS_FAIL_NONE;
    h->tls_error_code        = 0;
}

void bb_mqtt_host_simulate_reconnect(bb_mqtt_t handle)
{
    if (!handle) return;
    bb_mqtt_host_handle_t *h = (bb_mqtt_host_handle_t *)handle;
    if (!h->ever_connected) {
        h->ever_connected = true;
    }
    h->reconnect_count++;
}

void bb_mqtt_host_set_disc_reason(bb_mqtt_t handle, bb_mqtt_disc_t reason)
{
    if (!handle) return;
    ((bb_mqtt_host_handle_t *)handle)->disc_reason = reason;
}

void bb_mqtt_host_set_tls_fail(bb_mqtt_t handle, bb_tls_fail_t fail)
{
    if (!handle) return;
    ((bb_mqtt_host_handle_t *)handle)->tls_fail = fail;
}

void bb_mqtt_host_set_tls_error_code(bb_mqtt_t handle, int code)
{
    if (!handle) return;
    ((bb_mqtt_host_handle_t *)handle)->tls_error_code = code;
}

void bb_mqtt_default_set(bb_mqtt_t h)
{
    s_default_handle = h;
}

bool bb_mqtt_host_is_suspended_default(void)
{
    return s_suspended;
}

void bb_mqtt_host_set_stop_only(bool stop_only)
{
    s_stop_only = stop_only;
}

void bb_mqtt_set_calloc(void *(*fn)(size_t, size_t))
{
    s_calloc_fn = fn;
}

void bb_mqtt_host_inject_message(bb_mqtt_t handle, const char *topic,
                                  const void *payload, size_t len)
{
    bb_mqtt_host_inject_fragment(handle, topic, len, 0, payload, len);
}

void bb_mqtt_host_inject_fragment(bb_mqtt_t handle, const char *topic,
                                   size_t total_len, size_t offset,
                                   const void *data, size_t data_len)
{
    if (!handle || !topic) return;
    bb_mqtt_host_handle_t *h = (bb_mqtt_host_handle_t *)handle;
    if (!h->msg_cb || !h->reasm.buf) return;

    // Bound-copy the topic through a fixed buffer mirroring
    // BB_MQTT_SUB_TOPIC_MAX-style truncation, for host/device parity — the
    // caller's topic pointer is otherwise unbounded (LOW fix).
    char topic_bounded[BB_MQTT_REASM_TOPIC_MAX];
    snprintf(topic_bounded, sizeof(topic_bounded), "%s", topic);

    bool complete = bb_mqtt_reasm_step(&h->reasm, topic_bounded,
                                        strlen(topic_bounded),
                                        total_len, offset, data, data_len);
    if (complete) {
        h->msg_cb(h->reasm.topic, h->reasm.buf, h->reasm.len, h->msg_ctx);
    }
}

void bb_mqtt_host_set_subscribe_fail(bb_mqtt_t handle, bool fail)
{
    if (!handle) return;
    ((bb_mqtt_host_handle_t *)handle)->test_subscribe_fail = fail;
}

#endif /* BB_MQTT_TESTING */

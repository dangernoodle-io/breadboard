// bb_mqtt host stub — in-memory implementation for unit testing.
//
// bb_mqtt_init validates cfg and allocates a handle.
// bb_mqtt_publish records the last (and all) publish calls in a ring.
// bb_mqtt_is_connected returns a flag settable via bb_mqtt_host_set_connected.
// All operations return BB_OK; no real network IO occurs.
#include "bb_mqtt.h"
#include "bb_nv.h"

#include <stdlib.h>
#include <string.h>

#define BB_MQTT_HOST_PUB_CAP 32

typedef struct {
    bb_mqtt_host_pub_t pubs[BB_MQTT_HOST_PUB_CAP];
    int                count;
    bool               connected;
    bool               tls;             // captured from cfg.tls at init time
    bool               ever_connected;  // set when connected goes true for first time
    uint32_t           reconnect_count;
    uint8_t            last_disc_error_type;
} bb_mqtt_host_handle_t;

bb_err_t bb_mqtt_init(const bb_mqtt_cfg_t *cfg, bb_mqtt_t *out)
{
    if (!cfg || !out) return BB_ERR_INVALID_ARG;

    bb_mqtt_host_handle_t *h = calloc(1, sizeof(*h));
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
    out->reconnect_count      = h->reconnect_count;
    out->last_disc_error_type = h->last_disc_error_type;
    out->connected            = h->connected;
    return BB_OK;
}

bb_err_t bb_mqtt_destroy(bb_mqtt_t handle)
{
    if (!handle) return BB_OK;
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
// Models the ESP-IDF full-release semantics:
//  - suspend: DESTROYS the default handle (frees, NULLs s_default_handle),
//    sets s_suspended flag.  Mirrors the ~11 KB free on device.
//  - resume: RECREATES a fresh handle (connected=true, pub ring cleared),
//    restores s_default_handle, clears s_suspended.
//
// bb_mqtt_host_is_suspended_default() reports s_suspended for test assertions.
// ---------------------------------------------------------------------------

static bool s_suspended = false;

// NVS namespace/key constants (host: mirrored from espidf backend).
#define BB_MQTT_NVS_NS  "bb_mqtt"
#define BB_MQTT_URI_MAX 128

bb_err_t bb_mqtt_suspend_default(void)
{
    if (s_suspended) return BB_OK;   // idempotent

    // Destroy the current handle to model full release.
    if (s_default_handle) {
        bb_mqtt_destroy(s_default_handle);
        s_default_handle = NULL;
    }

    s_suspended = true;
    return BB_OK;
}

bb_err_t bb_mqtt_resume_default(void)
{
    if (!s_suspended) return BB_OK;   // idempotent

    // Recreate a fresh handle to model recreate-from-NVS.
    // On host: read NVS uri (or use a fallback) and create a new handle.
    char uri[BB_MQTT_URI_MAX] = {0};
    bb_nv_get_str(BB_MQTT_NVS_NS, "uri", uri, sizeof(uri), "");

    // If NVS has no uri (typical in unit tests), use a placeholder so init
    // succeeds — the host stub ignores the uri value for any real network ops.
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
    h->last_disc_error_type  = 0;
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

void bb_mqtt_host_set_last_disc_error_type(bb_mqtt_t handle, uint8_t error_type)
{
    if (!handle) return;
    ((bb_mqtt_host_handle_t *)handle)->last_disc_error_type = error_type;
}

void bb_mqtt_default_set(bb_mqtt_t h)
{
    s_default_handle = h;
}

bool bb_mqtt_host_is_suspended_default(void)
{
    return s_suspended;
}

#endif /* BB_MQTT_TESTING */

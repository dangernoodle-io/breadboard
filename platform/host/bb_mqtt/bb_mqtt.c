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
    bool               tls;       // captured from cfg.tls at init time
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
    (void)handle;
    (void)topic;
    (void)qos;
    return BB_OK;
}

bool bb_mqtt_is_connected(bb_mqtt_t handle)
{
    if (!handle) return false;
    return ((bb_mqtt_host_handle_t *)handle)->connected;
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
    h->count     = 0;
    h->connected = true;
}

void bb_mqtt_default_set(bb_mqtt_t h)
{
    s_default_handle = h;
}

#endif /* BB_MQTT_TESTING */

// bb_mqtt host stub — in-memory implementation for unit testing.
//
// bb_mqtt_init validates cfg and allocates a handle.
// bb_mqtt_publish records the last (and all) publish calls in a ring.
// bb_mqtt_is_connected returns a flag settable via bb_mqtt_host_set_connected.
// All operations return BB_OK; no real network IO occurs.
//
// bb_mqtt_reconfigure mirrors the ESP-IDF split behavior (B1-276): it peeks
// NVS "bb_mqtt"/"enabled" to decide whether this is a disable (teardown-only)
// or enable (init+start) call, and records that distinction so tests can assert
// via bb_mqtt_test_last_was_disable().
#include "bb_mqtt.h"
#include "bb_nv.h"

#include <stdlib.h>
#include <string.h>

#define BB_MQTT_HOST_PUB_CAP 32

typedef struct {
    bb_mqtt_host_pub_t pubs[BB_MQTT_HOST_PUB_CAP];
    int                count;
    bool               connected;
} bb_mqtt_host_handle_t;

bb_err_t bb_mqtt_init(const bb_mqtt_cfg_t *cfg, bb_mqtt_t *out)
{
    if (!cfg || !out) return BB_ERR_INVALID_ARG;

    bb_mqtt_host_handle_t *h = calloc(1, sizeof(*h));
    if (!h) return BB_ERR_NO_SPACE;

    // Simulate "connected" by default so publish tests don't need to set flag.
    h->connected = true;

    *out = h;
    return BB_OK;
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
// bb_mqtt_reconfigure — host stub
//
// Mirrors the ESP-IDF split behavior (B1-276): peeks NVS "bb_mqtt"/"enabled"
// to classify the call as disable (teardown-only) or enable (init+start).
// Records both the total count and the last-was-disable flag so tests can
// assert the correct path was taken without a real MQTT connection.
// ---------------------------------------------------------------------------

static int  s_reconfigure_count    = 0;
static bool s_last_reconfigure_was_disable = false;

bb_err_t bb_mqtt_reconfigure(void)
{
    // Peek enabled flag — mirrors the ESP-IDF split in bb_mqtt_reconfigure.
    char enabled_str[4] = "0";
    bb_nv_get_str("bb_mqtt", "enabled", enabled_str, sizeof(enabled_str), "0");
    s_last_reconfigure_was_disable = (enabled_str[0] != '1');

    s_reconfigure_count++;
    // On the host there is no managed client to tear down/restart.
    // We re-read NVS to mirror the ESP-IDF path; the observable effects are
    // the counter increment and the disable/enable classification.
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
    h->count     = 0;
    h->connected = true;
    s_reconfigure_count             = 0;
    s_last_reconfigure_was_disable  = false;
}

void bb_mqtt_default_set(bb_mqtt_t h)
{
    s_default_handle = h;
}

int bb_mqtt_test_reconfigure_count(void)
{
    return s_reconfigure_count;
}

bool bb_mqtt_test_last_reconfigure_was_disable(void)
{
    return s_last_reconfigure_was_disable;
}

#endif /* BB_MQTT_TESTING */

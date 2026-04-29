#include "bb_nv.h"
#include "bb_mdns.h"
#include "bb_mdns_host_test_hooks.h"
#include "bb_log.h"
#include <string.h>

static const char *TAG = "bb_mdns";

/* Counters for host test assertions. */
static int s_announce_count = 0;
static int s_set_txt_count  = 0;

int bb_mdns_host_announce_count(void) { return s_announce_count; }
int bb_mdns_host_set_txt_count(void)  { return s_set_txt_count; }
void bb_mdns_host_reset(void)
{
    s_announce_count = 0;
    s_set_txt_count  = 0;
}

/* Subscription table — mirrors espidf implementation for dispatch testing. */
#define BB_MDNS_HOST_BROWSE_MAX 4
typedef struct {
    char service[32];
    char proto[8];
    bb_mdns_peer_cb on_peer;
    bb_mdns_peer_removed_cb on_removed;
    void *ctx;
    bool in_use;
} bb_mdns_host_sub_t;
static bb_mdns_host_sub_t s_host_subs[BB_MDNS_HOST_BROWSE_MAX];

static bb_mdns_host_sub_t *host_sub_find(const char *service, const char *proto)
{
    for (int i = 0; i < BB_MDNS_HOST_BROWSE_MAX; i++) {
        if (s_host_subs[i].in_use &&
            strcmp(s_host_subs[i].service, service) == 0 &&
            strcmp(s_host_subs[i].proto,   proto)   == 0) {
            return &s_host_subs[i];
        }
    }
    return NULL;
}

/* ESP_PLATFORM-guarded stubs — no timer coalescing on host.
 * Explicit bb_mdns_announce() is what's testable; timer-based coalesce
 * is ESP-IDF hardware behaviour and is verified by flashing. */
void bb_mdns_set_txt(const char *key, const char *value)
{
    if (!key || !value) return;
    s_set_txt_count++;
    bb_log_d(TAG, "set_txt stub: %s=%s", key, value);
}

void bb_mdns_announce(void)
{
    s_announce_count++;
    bb_log_d(TAG, "announce stub");
}

bb_err_t bb_mdns_browse_start(const char *service, const char *proto,
                              bb_mdns_peer_cb on_peer,
                              bb_mdns_peer_removed_cb on_removed,
                              void *ctx)
{
    if (!service || !proto) {
        return BB_ERR_INVALID_ARG;
    }

    // Update existing subscription (idempotent)
    bb_mdns_host_sub_t *existing = host_sub_find(service, proto);
    if (existing) {
        existing->on_peer    = on_peer;
        existing->on_removed = on_removed;
        existing->ctx        = ctx;
        return BB_OK;
    }

    // Allocate new slot
    for (int i = 0; i < BB_MDNS_HOST_BROWSE_MAX; i++) {
        if (!s_host_subs[i].in_use) {
            strncpy(s_host_subs[i].service, service, sizeof(s_host_subs[i].service) - 1);
            s_host_subs[i].service[sizeof(s_host_subs[i].service) - 1] = '\0';
            strncpy(s_host_subs[i].proto, proto, sizeof(s_host_subs[i].proto) - 1);
            s_host_subs[i].proto[sizeof(s_host_subs[i].proto) - 1] = '\0';
            s_host_subs[i].on_peer    = on_peer;
            s_host_subs[i].on_removed = on_removed;
            s_host_subs[i].ctx        = ctx;
            s_host_subs[i].in_use     = true;
            bb_log_d(TAG, "browse_start stub: %s.%s", service, proto);
            return BB_OK;
        }
    }
    return BB_ERR_NO_SPACE;
}

bb_err_t bb_mdns_browse_stop(const char *service, const char *proto)
{
    if (!service || !proto) {
        return BB_ERR_INVALID_ARG;
    }

    bb_mdns_host_sub_t *sub = host_sub_find(service, proto);
    if (sub) {
        memset(sub, 0, sizeof(*sub));
    }
    bb_log_d(TAG, "browse_stop stub: %s.%s", service, proto);
    return BB_OK;
}

bb_err_t bb_mdns_query_txt(const char *instance_name, const char *service, const char *proto,
                           uint32_t timeout_ms, bb_mdns_query_cb cb, void *ctx)
{
    (void)timeout_ms;
    if (!instance_name || !service || !proto) return BB_ERR_INVALID_ARG;
    (void)cb;
    (void)ctx;
    return BB_OK;  /* host: never delivers a callback; tests use dispatch hook */
}

/* Test hooks: directly invoke the registered callbacks without a queue/task.
 * This lets host tests exercise the dispatch path synchronously. */

bb_err_t bb_mdns_host_dispatch_peer(const char *service, const char *proto,
                                    const bb_mdns_peer_t *peer)
{
    if (!service || !proto || !peer) return BB_ERR_INVALID_ARG;
    bb_mdns_host_sub_t *sub = host_sub_find(service, proto);
    if (!sub) return BB_OK;
    if (sub->on_peer) sub->on_peer(peer, sub->ctx);
    return BB_OK;
}

bb_err_t bb_mdns_host_dispatch_removed(const char *service, const char *proto,
                                       const char *instance_name)
{
    if (!service || !proto || !instance_name) return BB_ERR_INVALID_ARG;
    bb_mdns_host_sub_t *sub = host_sub_find(service, proto);
    if (!sub) return BB_OK;
    if (sub->on_removed) sub->on_removed(instance_name, sub->ctx);
    return BB_OK;
}

bb_err_t bb_mdns_host_dispatch_query_result(const bb_mdns_query_result_t *result,
                                            bb_mdns_query_cb cb, void *ctx)
{
    if (!result) return BB_ERR_INVALID_ARG;
    if (cb) cb(result, ctx);
    return BB_OK;
}

const char *bb_mdns_get_hostname(void)
{
    return NULL;  /* host: no cached hostname */
}

#include "bb_nv.h"
#include "bb_mdns.h"
#include "bb_mdns_host_test_hooks.h"
#include "bb_log.h"
#include <stdbool.h>
#include <string.h>

#ifdef BB_MDNS_TESTING
#include "bb_mdns_test.h"
#endif

static const char *TAG = "bb_mdns";

/* One-time warning: mDNS is unsupported on host (no network stack). */
static bool s_warned = false;

/* Counters for host test assertions. */
static int s_announce_count = 0;
static int s_set_txt_count  = 0;

int bb_mdns_host_announce_count(void) { return s_announce_count; }
int bb_mdns_host_set_txt_count(void)  { return s_set_txt_count; }
bool bb_mdns_host_warned(void)         { return s_warned; }
void bb_mdns_host_reset(void)
{
    s_announce_count = 0;
    s_set_txt_count  = 0;
    s_warned         = false;
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
    if (!s_warned) {
        bb_log_w(TAG, "mDNS not supported on host platform; calls are no-ops");
        s_warned = true;
    }
    s_set_txt_count++;
    bb_log_d(TAG, "set_txt stub: %s=%s", key, value);
}

void bb_mdns_deinit(void)
{
    bb_log_d(TAG, "deinit stub");
}

void bb_mdns_start(void)
{
    bb_log_d(TAG, "start stub");
}

void bb_mdns_announce(void)
{
    if (!s_warned) {
        bb_log_w(TAG, "mDNS not supported on host platform; calls are no-ops");
        s_warned = true;
    }
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
    if (!s_warned) {
        bb_log_w(TAG, "mDNS not supported on host platform; calls are no-ops");
        s_warned = true;
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
    if (!s_warned) {
        bb_log_w(TAG, "mDNS not supported on host platform; calls are no-ops");
        s_warned = true;
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
    if (!s_warned) {
        bb_log_w(TAG, "mDNS not supported on host platform; calls are no-ops");
        s_warned = true;
    }
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

bool bb_mdns_started(void)
{
    return false;  /* host: mDNS not running */
}

// ---------------------------------------------------------------------------
// Coalesce test hooks (BB_MDNS_TESTING only)
// ---------------------------------------------------------------------------
// These mirror the batch-append / flush logic in platform/espidf/bb_mdns/bb_mdns.c
// but run on host without IDF deps (no mutexes, no esp_timer).  Tests drive
// bb_mdns_coalesce_append_for_test() to inject synthetic peer events into the
// batch, then call bb_mdns_coalesce_flush_for_test() to simulate the timer
// firing.  Assertions verify the single-flush contract and per-peer callbacks.

#ifdef BB_MDNS_TESTING

#define BB_MDNS_HOST_BATCH_MAX 16

typedef struct {
    bb_mdns_peer_t entries[BB_MDNS_HOST_BATCH_MAX];
    bool           is_removal[BB_MDNS_HOST_BATCH_MAX];
    char           instance_names[BB_MDNS_HOST_BATCH_MAX][BB_MDNS_INSTANCE_NAME_MAX];
    char           service[BB_MDNS_HOST_BATCH_MAX][32];
    char           proto[BB_MDNS_HOST_BATCH_MAX][8];
    int            count;
    int            flush_count;          /* how many times flush was called */
    int            queue_enqueue_count;  /* items posted to the simulated dispatch queue */
    int            queue_depth;          /* current simulated queue depth */
    int            queue_depth_cap;      /* 0 = unlimited; >0 = max queue depth */
    int            drop_count;           /* queue+batch full drops */
} bb_mdns_coalesce_state_t;

static bb_mdns_coalesce_state_t s_coalesce;

void bb_mdns_coalesce_reset_for_test(void)
{
    memset(&s_coalesce, 0, sizeof(s_coalesce));
}

int bb_mdns_coalesce_batch_count(void)
{
    return s_coalesce.count;
}

int bb_mdns_coalesce_flush_count(void)
{
    return s_coalesce.flush_count;
}

int bb_mdns_coalesce_queue_enqueue_count(void)
{
    return s_coalesce.queue_enqueue_count;
}

int bb_mdns_coalesce_drop_count(void)
{
    return s_coalesce.drop_count;
}

void bb_mdns_coalesce_queue_depth_cap_set_for_test(int cap)
{
    s_coalesce.queue_depth_cap = cap;
}

void bb_mdns_coalesce_queue_depth_hold_for_test(int depth)
{
    s_coalesce.queue_depth = depth;
}

void bb_mdns_coalesce_queue_drain_for_test(void)
{
    s_coalesce.queue_depth = 0;
}

/* Internal: flush current batch to simulated queue.  Returns true on success,
 * false if queue is full (cap exceeded).  Dispatches callbacks on success. */
static bool coalesce_do_flush(void)
{
    int n = s_coalesce.count;
    if (n == 0) return true;  /* empty — nothing to do */

    /* Check queue depth cap. */
    if (s_coalesce.queue_depth_cap > 0 &&
        s_coalesce.queue_depth >= s_coalesce.queue_depth_cap) {
        /* Queue full — clear the batch (events are lost) and report failure. */
        s_coalesce.count = 0;
        return false;
    }

    s_coalesce.queue_enqueue_count++;
    s_coalesce.queue_depth++;

    for (int i = 0; i < n; i++) {
        const char *svc   = s_coalesce.service[i];
        const char *proto = s_coalesce.proto[i];
        if (s_coalesce.is_removal[i]) {
            bb_mdns_host_dispatch_removed(svc, proto, s_coalesce.instance_names[i]);
        } else {
            bb_mdns_host_dispatch_peer(svc, proto, &s_coalesce.entries[i]);
        }
    }
    s_coalesce.count = 0;
    /* Simulate dispatcher freeing the item immediately (no real task delay). */
    s_coalesce.queue_depth--;
    return true;
}

/* Mirror of the ESP-IDF batch_append_locked logic:
 * - If batch is full, flush synchronously before appending.
 * - If flush fails (queue full), drop the event.
 * Returns BB_OK on success, BB_ERR_NO_SPACE on queue+batch overload. */
bb_err_t bb_mdns_coalesce_append_for_test(const char *service, const char *proto,
                                          const bb_mdns_peer_t *peer, bool is_removal)
{
    if (!service || !proto) return BB_ERR_INVALID_ARG;

    if (s_coalesce.count >= BB_MDNS_HOST_BATCH_MAX) {
        /* Batch full — flush synchronously (mirrors batch_append_locked). */
        if (!coalesce_do_flush()) {
            /* Queue also full — real overload, drop the event. */
            s_coalesce.drop_count++;
            return BB_ERR_NO_SPACE;
        }
        /* Flush succeeded; batch.count is 0.  Fall through to append. */
    }

    int i = s_coalesce.count;
    if (peer) {
        s_coalesce.entries[i] = *peer;
    } else {
        memset(&s_coalesce.entries[i], 0, sizeof(bb_mdns_peer_t));
    }
    s_coalesce.is_removal[i] = is_removal;
    strncpy(s_coalesce.service[i], service, sizeof(s_coalesce.service[i]) - 1);
    s_coalesce.service[i][sizeof(s_coalesce.service[i]) - 1] = '\0';
    strncpy(s_coalesce.proto[i], proto, sizeof(s_coalesce.proto[i]) - 1);
    s_coalesce.proto[i][sizeof(s_coalesce.proto[i]) - 1] = '\0';
    if (peer && peer->instance_name[0] != '\0') {
        strncpy(s_coalesce.instance_names[i], peer->instance_name,
                sizeof(s_coalesce.instance_names[i]) - 1);
    }
    s_coalesce.count++;
    return BB_OK;
}

/* Simulate timer fire: dispatch all pending batch entries to registered callbacks,
 * then reset the batch.  Returns the number of entries dispatched. */
int bb_mdns_coalesce_flush_for_test(void)
{
    s_coalesce.flush_count++;
    int n = s_coalesce.count;
    if (n == 0) return 0;

    coalesce_do_flush();
    return n;
}

#endif /* BB_MDNS_TESTING */

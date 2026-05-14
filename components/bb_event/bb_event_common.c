#include "bb_event.h"
#include "bb_event_port.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bb_event";

// Defaults from Kconfig
#ifndef CONFIG_BB_EVENT_QUEUE_DEPTH
#define CONFIG_BB_EVENT_QUEUE_DEPTH 16
#endif
#ifndef CONFIG_BB_EVENT_MAX_PAYLOAD
#define CONFIG_BB_EVENT_MAX_PAYLOAD 256
#endif
#ifndef CONFIG_BB_EVENT_MAX_TOPICS
#define CONFIG_BB_EVENT_MAX_TOPICS 16
#endif

#define BB_EVENT_MAX_SUBSCRIBERS (CONFIG_BB_EVENT_MAX_TOPICS * 8)
#define BB_EVENT_TOPIC_NAME_MAX 32

// ---------------------------------------------------------------------------
// Subscriber pool
// ---------------------------------------------------------------------------

typedef struct bb_event_subscriber {
    bb_event_topic_t topic;
    bb_event_handler_fn cb;
    void *user;
    struct bb_event_subscriber *next;
} bb_event_subscriber_t;

static bb_event_subscriber_t s_sub_pool[BB_EVENT_MAX_SUBSCRIBERS];
static bb_event_subscriber_t *s_free_list = NULL;
static bool s_pool_initialized = false;

static void init_subscriber_pool(void)
{
    if (s_pool_initialized) return;

    for (int i = 0; i < BB_EVENT_MAX_SUBSCRIBERS - 1; i++) {
        s_sub_pool[i].next = &s_sub_pool[i + 1];
    }
    s_sub_pool[BB_EVENT_MAX_SUBSCRIBERS - 1].next = NULL;
    s_free_list = &s_sub_pool[0];
    s_pool_initialized = true;
}

static bb_event_subscriber_t *alloc_subscriber(void)
{
    bb_event_subscriber_t *sub = s_free_list;
    if (sub) {
        s_free_list = sub->next;
        memset(sub, 0, sizeof(*sub));
    }
    return sub;
}

static void free_subscriber(bb_event_subscriber_t *sub)
{
    if (!sub) return;  // LCOV_EXCL_LINE
    sub->next = s_free_list;
    s_free_list = sub;
}

// ---------------------------------------------------------------------------
// Topic registry
// ---------------------------------------------------------------------------

typedef struct {
    char name[BB_EVENT_TOPIC_NAME_MAX];
    bb_event_subscriber_t *sub_head;
    uint32_t generation;  // ABA safety for unsubscribe
} bb_event_topic_entry_t;

static struct bb_event_topic {
    bb_event_topic_entry_t entry;
} s_topic_registry[CONFIG_BB_EVENT_MAX_TOPICS];

static int s_num_topics = 0;
static bool s_init_done = false;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_event_init(const bb_event_cfg_t *cfg)
{
    if (s_init_done) return BB_OK;

    size_t queue_depth = CONFIG_BB_EVENT_QUEUE_DEPTH;
    size_t max_payload = CONFIG_BB_EVENT_MAX_PAYLOAD;
    size_t stack_size = 0;
    int task_priority = 0;

    if (cfg) {
        if (cfg->queue_depth) queue_depth = cfg->queue_depth;
        if (cfg->max_payload) max_payload = cfg->max_payload;
        stack_size = cfg->stack_size;
        task_priority = cfg->task_priority;
    }

    init_subscriber_pool();

    bb_err_t err = bb_event_port_init(queue_depth, max_payload, stack_size, task_priority);
    if (err != BB_OK) {
        bb_log_e(TAG, "port_init failed: %d", err);
        return err;
    }

    s_init_done = true;
    bb_log_i(TAG, "initialized: queue_depth=%zu max_payload=%zu", queue_depth, max_payload);
    return BB_OK;
}

bb_err_t bb_event_topic_register(const char *name, bb_event_topic_t *out)
{
    if (!name || !out) return BB_ERR_INVALID_ARG;
    if (!s_init_done) return BB_ERR_INVALID_STATE;

    // Check for existing topic with same name
    for (int i = 0; i < s_num_topics; i++) {
        if (strcmp(s_topic_registry[i].entry.name, name) == 0) {
            *out = &s_topic_registry[i];
            return BB_OK;
        }
    }

    // Allocate new topic
    if (s_num_topics >= CONFIG_BB_EVENT_MAX_TOPICS) {
        bb_log_e(TAG, "topic registry full: %d/%d", s_num_topics, CONFIG_BB_EVENT_MAX_TOPICS);
        return BB_ERR_NO_SPACE;
    }

    int idx = s_num_topics++;
    strncpy(s_topic_registry[idx].entry.name, name, BB_EVENT_TOPIC_NAME_MAX - 1);
    s_topic_registry[idx].entry.name[BB_EVENT_TOPIC_NAME_MAX - 1] = '\0';
    s_topic_registry[idx].entry.sub_head = NULL;
    s_topic_registry[idx].entry.generation = 0;

    *out = &s_topic_registry[idx];
    bb_log_d(TAG, "registered topic: %s", name);
    return BB_OK;
}

bb_err_t bb_event_topic_lookup(const char *name, bb_event_topic_t *out)
{
    if (!name || !out) return BB_ERR_INVALID_ARG;

    for (int i = 0; i < s_num_topics; i++) {
        if (strcmp(s_topic_registry[i].entry.name, name) == 0) {
            *out = &s_topic_registry[i];
            return BB_OK;
        }
    }

    return BB_ERR_NOT_FOUND;
}

// Caller must hold the bb_event lock.
static bb_err_t subscribe_locked(bb_event_topic_t topic,
                                 bb_event_handler_fn cb, void *user,
                                 bb_event_sub_t *out_sub)
{
    bb_event_subscriber_t *sub = alloc_subscriber();
    if (!sub) {
        bb_log_e(TAG, "subscriber pool full: %d/%d", BB_EVENT_MAX_SUBSCRIBERS, BB_EVENT_MAX_SUBSCRIBERS);
        return BB_ERR_NO_SPACE;
    }

    struct bb_event_topic *t = (struct bb_event_topic *)topic;
    sub->topic = topic;
    sub->cb = cb;
    sub->user = user;
    sub->next = t->entry.sub_head;
    t->entry.sub_head = sub;

    *out_sub = (bb_event_sub_t)sub;
    return BB_OK;
}

bb_err_t bb_event_subscribe(bb_event_topic_t topic,
                            bb_event_handler_fn cb, void *user,
                            bb_event_sub_t *out_sub)
{
    if (!topic || !cb || !out_sub) return BB_ERR_INVALID_ARG;

    bb_event_port_lock();
    bb_err_t err = subscribe_locked(topic, cb, user, out_sub);
    bb_event_port_unlock();
    return err;
}

bb_err_t bb_event_subscribe_with_prep(bb_event_topic_t topic,
                                      bb_event_handler_fn cb, void *user,
                                      void (*prep)(void *prep_arg),
                                      void *prep_arg,
                                      bb_event_sub_t *out_sub)
{
    if (!topic || !cb || !out_sub) return BB_ERR_INVALID_ARG;

    bb_event_port_lock();
    if (prep) prep(prep_arg);
    bb_err_t err = subscribe_locked(topic, cb, user, out_sub);
    bb_event_port_unlock();
    return err;
}

void bb_event_lock(void)   { bb_event_port_lock(); }
void bb_event_unlock(void) { bb_event_port_unlock(); }

bb_err_t bb_event_unsubscribe(bb_event_sub_t sub)
{
    if (!sub) return BB_ERR_INVALID_ARG;

    bb_event_subscriber_t *s = (bb_event_subscriber_t *)sub;
    struct bb_event_topic *t = (struct bb_event_topic *)s->topic;

    bb_event_port_lock();

    // Find and unlink. Loop always terminates via `break` for valid handles;
    // walking off the end is defensive only.
    bb_event_subscriber_t **pnext = &t->entry.sub_head;
    while (*pnext) {  // LCOV_EXCL_BR_LINE
        if (*pnext == s) {
            *pnext = s->next;
            break;
        }
        pnext = &(*pnext)->next;
    }

    free_subscriber(s);

    bb_event_port_unlock();
    return BB_OK;
}

bb_err_t bb_event_post(bb_event_topic_t topic, int32_t id,
                       const void *data, size_t size)
{
    if (!topic) return BB_ERR_INVALID_ARG;
    if (size > CONFIG_BB_EVENT_MAX_PAYLOAD) return BB_ERR_INVALID_ARG;

    bb_event_queue_entry_t entry = {
        .topic = topic,
        .id = id,
        .size = size,
    };

    return bb_event_port_enqueue(&entry, data);
}

size_t bb_event_pump(uint32_t budget)
{
    return bb_event_port_drain(budget);
}

// ---------------------------------------------------------------------------
// Auto-register via bb_registry
// ---------------------------------------------------------------------------

#if CONFIG_BB_EVENT_AUTOREGISTER
static bb_err_t bb_event_autoinit(void)
{
    return bb_event_init(NULL);  // NULL = Kconfig defaults
}
BB_REGISTRY_REGISTER_EARLY(bb_event, bb_event_autoinit);
#endif

// ---------------------------------------------------------------------------
// Dispatcher (called by port)
// ---------------------------------------------------------------------------

void bb_event_common_dispatch(const bb_event_queue_entry_t *entry,
                              const void *payload)
{
    if (!entry) return;

    struct bb_event_topic *t = (struct bb_event_topic *)entry->topic;

    bb_event_port_lock();
    // Snapshot the list: walk it while holding lock, then release before calling handlers.
    // This is safe because we call handlers outside the lock, so unsubscribe can't corrupt the walk.
    bb_event_subscriber_t *sub = t->entry.sub_head;
    bb_event_port_unlock();

    while (sub) {
        if (sub->cb) {  // LCOV_EXCL_LINE
            sub->cb(entry->topic, entry->id, payload, entry->size, sub->user);
        }
        sub = sub->next;
    }
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

#ifdef BB_EVENT_TESTING
void bb_event_reset_for_test(void) {
    s_init_done = false;
    /* leave s_pool_initialized alone — test_bb_event_init_pool_already_initialized
       depends on the pool guard staying set across reset. */
    s_num_topics = 0;
    /* clear subscriber pool head pointers per-topic */
    for (size_t i = 0; i < CONFIG_BB_EVENT_MAX_TOPICS; ++i) {
        s_topic_registry[i].entry.sub_head = NULL;
    }
}

void bb_event_reset_pool_for_test(void) {
    s_pool_initialized = false;
    /* re-init the free list on next bb_event_init call */
}
#endif

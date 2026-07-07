#include "bb_mdns.h"
#include "bb_mdns_lifecycle.h"
#include "bb_mdns_refresh_decision.h"
#include "bb_wifi.h"
#include "bb_init.h"
#include "bb_http_server.h"
#include "bb_timer.h"
#include "bb_ring.h"
#include "mdns.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "bb_hw.h"
#include "bb_log.h"
#include "bb_mem.h"
#include "bb_task.h"
#include "bb_str.h"
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "bb_mdns";

// Browse subscription table
#define BB_MDNS_BROWSE_MAX 4
typedef struct {
    char service[32];
    char proto[8];
    bb_mdns_peer_cb on_peer;
    bb_mdns_peer_removed_cb on_removed;
    void *ctx;
    bool in_use;
} bb_mdns_browse_sub_t;
static bb_mdns_browse_sub_t s_subs[BB_MDNS_BROWSE_MAX];

// Dispatch worker infrastructure
static SemaphoreHandle_t s_ring_sem      = NULL;  // binary — signals dispatch task
static TaskHandle_t      s_dispatch_task = NULL;
static SemaphoreHandle_t s_subs_mutex    = NULL;
// Serializes lifecycle teardown across the wifi-disconnect callback and the
// esp_register_shutdown_handler path. Both can fire during esp_restart and the
// portable bb_mdns_lifecycle started→stopped transition is not atomic.
static SemaphoreHandle_t s_lifecycle_mutex = NULL;
static uint32_t         s_evt_drop_count = 0;
// B1-539: browse refresh cycles skipped because mdns_browse_delete's enqueue
// failed (queue full / no memory) — recreate is deferred to the next tick to
// avoid issuing mdns_browse_new against a not-yet-torn-down browse.
static uint32_t         s_refresh_skip_count = 0;

// Async TXT query infrastructure
#define BB_MDNS_QUERY_QUEUE_DEPTH 8

typedef struct {
    char instance_name[BB_MDNS_INSTANCE_NAME_MAX];
    char service[32];
    char proto[8];
    uint32_t timeout_ms;
    bb_mdns_query_cb cb;
    void *ctx;
} bb_mdns_query_req_t;

static QueueHandle_t s_query_queue = NULL;
static TaskHandle_t  s_query_task  = NULL;

// Event struct: single contiguous alloc; key/value in txt[] point into payload[]
#define BB_MDNS_EVT_TXT_MAX 8
typedef struct {
    bool     is_removal;
    char     service[32];
    char     proto[8];
    char     instance_name[BB_MDNS_INSTANCE_NAME_MAX];
    char     hostname[BB_MDNS_HOSTNAME_MAX];
    char     ip4[BB_MDNS_IP4_MAX];
    uint16_t port;
    size_t   txt_count;
    bb_mdns_txt_t txt[BB_MDNS_EVT_TXT_MAX];
    char     payload[256];   /* packed key\0value\0… */
} bb_mdns_evt_t;

// Mutex that protects the coalescing batch buffer (s_batch) and the batch
// item slot (s_batch_item).  Replaces the old per-event pool mutex; the
// Kconfig BB_MDNS_EVT_POOL_SIZE now only governs the dispatch queue depth
// rather than a pool of statically-allocated event structs.
static SemaphoreHandle_t s_evt_pool_lock = NULL;

// ---------------------------------------------------------------------------
// Coalescing batch buffer (producer-side)
// ---------------------------------------------------------------------------
// IDF's mDNS notifier fires once per peer in rapid succession when a
// browse-refresh scan returns multiple results.  On CPU-starved boards the
// dispatch task can't drain the per-event pool fast enough, causing pool
// exhaustion and dropped events.  Solution: buffer incoming peer events into
// a static batch and flush them as a single queue item after a short window.
//
// The pending batch and the flush timer are protected by s_evt_pool_lock
// (reusing the existing mutex avoids introducing a second lock).
//
// BB_MDNS_BATCH_MAX — Kconfig-driven on ESP-IDF; host fallback for unit tests.
// Sizes the s_batch static array (bb_mdns_evt_t[BB_MDNS_BATCH_MAX], ~516 B/slot).
// Default 16 → ~8.2 KB .bss.
#ifdef CONFIG_BB_MDNS_BATCH_MAX
#define BB_MDNS_BATCH_MAX CONFIG_BB_MDNS_BATCH_MAX
#else
#define BB_MDNS_BATCH_MAX 16
#endif

// BB_MDNS_BATCH_RING_DEPTH — capacity of the bb_ring dispatch queue.
// Heap cost (SPIRAM-preferred on ESP-IDF):
//   BB_MDNS_BATCH_RING_DEPTH × (bb_ring entry metadata (~24 B) + sizeof(bb_mdns_batch_item_t))
// At defaults (depth=4, BATCH_MAX=16): 4 × (24 + sizeof(bb_mdns_batch_item_t)) ≈ 33 KB on SPIRAM.
// On no-PSRAM boards the same bytes come from internal heap.
#ifdef CONFIG_BB_MDNS_BATCH_RING_DEPTH
#define BB_MDNS_BATCH_RING_DEPTH CONFIG_BB_MDNS_BATCH_RING_DEPTH
#else
#define BB_MDNS_BATCH_RING_DEPTH 4
#endif

// FreeRTOS priority for the dispatch + query tasks (Kconfig BB_MDNS_TASK_PRIORITY).
// Single-core boards (C3/S2) raise this so the dispatch task is not starved by
// the miner and drops coalesced browse-notify bursts; see Kconfig help.
#ifdef CONFIG_BB_MDNS_TASK_PRIORITY
#define BB_MDNS_TASK_PRIO CONFIG_BB_MDNS_TASK_PRIORITY
#else
#define BB_MDNS_TASK_PRIO 3
#endif

typedef struct {
    bb_mdns_evt_t entries[BB_MDNS_BATCH_MAX];
    int           count;
} bb_mdns_batch_t;

// Batched queue item stored by value in the bb_ring dispatch ring.
// Each flush serializes the pending coalescing batch into one of these
// and pushes it into s_batch_ring; the dispatcher peek/pops from the ring.
typedef struct {
    int           count;
    bb_mdns_evt_t entries[BB_MDNS_BATCH_MAX];
} bb_mdns_batch_item_t;

static bb_mdns_batch_t   s_batch;
static bb_oneshot_timer_t s_flush_timer = NULL;

// bb_ring holding bb_mdns_batch_item_t values, created in bb_mdns_init.
// BB_RING_REJECT_NEW policy: when the ring is full the flush timer re-arms
// and retries on the next 50 ms tick; drop-new is correct here.
static bb_ring_t s_batch_ring = NULL;

// Heap-allocated scratch item used by batch_do_flush_locked and the dispatch
// task.  Allocated once in bb_mdns_init; size is ~sizeof(bb_mdns_batch_item_t)
// (~8260 B at BATCH_MAX=16).  Keeping it off any task's stack prevents stack
// overflow on the dispatch task (4096 B budget) and on flush callers.
// On no-PSRAM boards this lands in internal heap; on PSRAM boards bb_ring's
// allocator path may redirect it to SPIRAM.
static bb_mdns_batch_item_t *s_dispatch_item = NULL;

// Monotonic sequence counter for ring push metadata (diagnostic id field).
static uint32_t s_push_seq = 0;

// Flush the current batch synchronously. Called under s_evt_pool_lock.
// Serializes the pending coalescing batch into s_dispatch_item, txt-pointer-
// relocates into the item's own payload copy, then pushes it by value into
// s_batch_ring.
// On BB_ERR_NO_SPACE (ring full): increments drop counter, logs, leaves
// s_batch INTACT so the 50 ms retry timer genuinely re-attempts the SAME batch.
// Releases + re-acquires s_evt_pool_lock around the ring push so the
// dispatcher task can drain the ring while the lock is free.
static bool batch_do_flush_locked(void)
{
    if (s_batch.count == 0) return true;  /* empty — nothing to do */

    s_dispatch_item->count = s_batch.count;
    memcpy(s_dispatch_item->entries, s_batch.entries,
           (size_t)s_batch.count * sizeof(bb_mdns_evt_t));

    /* The memcpy duplicated each entry's payload[] but left its txt[].key/value
     * pointers aimed at the SOURCE s_batch payload. s_batch is a persistent
     * static buffer reused by the next browse burst before the (single-core
     * starved) dispatcher consumes this item — reading through the stale
     * pointers then yields another peer's TXT (cross-attributed worker/version,
     * or blanks). Relocate each pointer into the item's own payload copy. */
    for (int e = 0; e < s_dispatch_item->count; e++) {
        intptr_t off = (intptr_t)s_dispatch_item->entries[e].payload -
                       (intptr_t)s_batch.entries[e].payload;
        for (size_t t = 0; t < s_dispatch_item->entries[e].txt_count && t < BB_MDNS_EVT_TXT_MAX; t++) {
            if (s_dispatch_item->entries[e].txt[t].key)
                s_dispatch_item->entries[e].txt[t].key   = (char *)((intptr_t)s_dispatch_item->entries[e].txt[t].key   + off);
            if (s_dispatch_item->entries[e].txt[t].value)
                s_dispatch_item->entries[e].txt[t].value = (char *)((intptr_t)s_dispatch_item->entries[e].txt[t].value + off);
        }
    }

    /* Release the lock so the dispatcher can drain the ring while we push. */
    xSemaphoreGive(s_evt_pool_lock);

    int64_t  ts  = (int64_t)bb_timer_now_us();
    uint32_t seq = ++s_push_seq;
    bb_err_t err = bb_ring_push(s_batch_ring, s_dispatch_item,
                                sizeof(*s_dispatch_item), ts, seq);

    xSemaphoreTake(s_evt_pool_lock, portMAX_DELAY);

    if (err == BB_OK) {
        /* Batch consumed — clear it now that the push succeeded. */
        s_batch.count = 0;
        /* Signal the dispatch task there is at least one item ready. */
        xSemaphoreGive(s_ring_sem);
        return true;
    }

    /* BB_ERR_NO_SPACE: ring full (drop-new policy).  Leave s_batch intact so
     * the 50 ms retry timer re-attempts the SAME batch rather than silently
     * dropping it.  Increment the drop counter for observability only. */
    s_evt_drop_count++;
    if ((s_evt_drop_count & 0x0F) == 1) {
        bb_log_w(TAG, "flush: ring full, will retry in 50 ms (%" PRIu32 " drop events total)",
                 s_evt_drop_count);
    }
    return false;
}

// Flush work function: runs on bb_timer_disp task (off the esp_timer service task).
static void flush_work_fn(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_evt_pool_lock, portMAX_DELAY);

    if (s_batch.count == 0) {
        xSemaphoreGive(s_evt_pool_lock);
        return;
    }

    if (!batch_do_flush_locked()) {
        /* batch_do_flush_locked re-acquired the lock; ring was full.
         * s_batch is INTACT (not cleared) — re-arm the 50 ms timer so the
         * same batch is retried once the dispatcher drains at least one slot. */
        xSemaphoreGive(s_evt_pool_lock);
        bb_timer_oneshot_start(s_flush_timer, 50 * 1000);
        return;
    }

    xSemaphoreGive(s_evt_pool_lock);
}

static void flush_timer_ensure_created(void)
{
    if (s_flush_timer) return;
    bb_err_t err = bb_timer_deferred_oneshot_create(flush_work_fn, NULL,
                                                     "bb_mdns_flush", &s_flush_timer);
    if (err != BB_OK) {
        bb_log_w(TAG, "bb_timer_deferred_oneshot_create(flush) failed: %d", (int)err);
    }
}

// Append a peer event to the pending batch (called under s_evt_pool_lock).
// Starts the one-shot flush timer on the first append to an empty batch.
//
// On overflow (batch full): synchronously flushes the current batch to the
// dispatcher queue, resets the batch, cancels the pending flush timer, appends
// the new event, and re-arms the flush timer.  This makes BB_MDNS_BATCH_MAX a
// max-batch-size knob rather than a drop threshold.
//
// Returns true if the event was appended; false only if both the batch item slot
// and the dispatcher queue were full simultaneously (queue+batch overload).
static bool batch_append_locked(const bb_mdns_evt_t *evt)
{
    if (s_batch.count >= BB_MDNS_BATCH_MAX) {
        /* Batch is full — flush synchronously before appending.
         * Cancel the pending timer first; the synchronous flush supersedes it. */
        if (s_flush_timer) bb_timer_oneshot_stop(s_flush_timer);  /* ignore if not running */

        if (!batch_do_flush_locked()) {
            /* Ring full — s_batch still holds the existing entries.  Cannot
             * append the new event now: both the ring and batch are full. */
            return false;  /* caller logs the drop */
        }
        /* Flush succeeded; batch.count is now 0.  Fall through to append. */
    }

    bb_mdns_evt_t *dst = &s_batch.entries[s_batch.count++];
    *dst = *evt;
    /* The struct copy duplicates payload[] but leaves txt[i].key/value
     * pointing into the source evt's payload. Relocate each pointer by the
     * offset between the source and destination payloads so the txt array
     * remains valid for the dispatcher (which fires after the source evt
     * has gone out of scope). */
    intptr_t off = (intptr_t)dst->payload - (intptr_t)evt->payload;
    for (size_t i = 0; i < dst->txt_count && i < BB_MDNS_EVT_TXT_MAX; i++) {
        if (dst->txt[i].key)   dst->txt[i].key   = (char *)((intptr_t)dst->txt[i].key   + off);
        if (dst->txt[i].value) dst->txt[i].value = (char *)((intptr_t)dst->txt[i].value + off);
    }
    if (s_batch.count == 1 && s_flush_timer) {
        /* First event in a new batch — arm the one-shot 50 ms flush window. */
        bb_timer_oneshot_start(s_flush_timer, 50 * 1000);  /* 50 ms */
    }
    return true;
}

// Forward declarations for browse refresh timer (used by bb_mdns_on_got_ip)
static void browse_refresh_timer_start(void);
static void browse_refresh_timer_stop(void);

// App-injected mDNS hostname
static char s_mdns_hostname[BB_MDNS_HOSTNAME_MAX] = "bsp-device";
static bool s_mdns_hostname_set = false;

// Cached running hostname (set during init)
static char s_running_hostname[BB_MDNS_HOSTNAME_MAX] = {0};
static bool s_running_hostname_valid = false;

// App-injected mDNS service type and instance name
static char s_mdns_service_type[32] = "_bsp";
static bool s_mdns_service_type_set = false;
static char s_mdns_instance_name[BB_MDNS_INSTANCE_NAME_MAX] = "BSP Device";
static bool s_mdns_instance_name_set = false;

// Lifecycle state machine
static bb_mdns_lifecycle_state_t s_lc = {0};

/* Coalescing re-announce timer: armed by bb_mdns_set_txt (post-start) and
 * bb_mdns_announce. Fires after BB_MDNS_ANNOUNCE_DELAY_US to emit a fresh
 * unsolicited mDNS announce — observers' IDF caches can miss TXT-only updates
 * without it. Restarting the timer on each set_txt call is intentional: the
 * last-write-wins coalesce delays the announce until the burst settles. */
#define BB_MDNS_ANNOUNCE_DELAY_US (100 * 1000)  /* 100 ms */
static bb_oneshot_timer_t s_announce_timer = NULL;

/* Periodic browse-refresh timer. IDF's mdns_browse only fires the notify
 * callback on PTR state changes; same-TTL re-announces by an active
 * advertiser are silently absorbed. To surface "still alive" without
 * patching IDF, periodically delete + re-create each browse, which forces
 * a fresh PTR scan and produces a new add notification per responder.
 * See B1-87. */
static bb_periodic_timer_t s_browse_refresh_timer = NULL;

static int apply_announce_locked(void)
{
    /* Use WIFI_STA_DEF netif — same key used by bb_wifi_init_sta. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) {
        bb_log_w(TAG, "announce: no STA netif");
        return -1;
    }
    esp_err_t err = mdns_netif_action(sta,
                                      MDNS_EVENT_ANNOUNCE_IP4 |
                                      MDNS_EVENT_ANNOUNCE_IP6);
    if (err != ESP_OK) {
        bb_log_w(TAG, "mdns_netif_action(ANNOUNCE) failed: %s", esp_err_to_name(err));
        return -1;
    }
    bb_log_d(TAG, "unsolicited re-announce sent");
    return 0;
}

static void announce_work_fn(void *arg)
{
    (void)arg;
    if (!bb_mdns_lifecycle_is_started(&s_lc)) return;
    if (!s_lc.announce_dirty) return;
    s_lc.announce_dirty = false;
    apply_announce_locked();
}

static void announce_timer_ensure_created(void)
{
    if (s_announce_timer) return;
    bb_err_t err = bb_timer_deferred_oneshot_create(announce_work_fn, NULL,
                                                     "bb_mdns_announce", &s_announce_timer);
    if (err != BB_OK) {
        bb_log_w(TAG, "bb_timer_deferred_oneshot_create(announce) failed: %d", (int)err);
    }
}

static void announce_arm(uint64_t delay_us)
{
    if (!s_announce_timer) return;
    bb_mdns_lifecycle_mark_dirty(&s_lc);
    /* bb_timer_oneshot_start re-arms even if already pending, implementing
     * last-write-wins coalescing. */
    bb_timer_oneshot_stop(s_announce_timer);  /* ignore error if not running */
    bb_err_t err = bb_timer_oneshot_start(s_announce_timer, delay_us);
    if (err != BB_OK) {
        bb_log_w(TAG, "announce timer arm failed: %d", (int)err);
    }
}

/* Persistent TXT cache: authoritative store for all set_txt calls. Every
 * bb_mdns_set_txt() update goes here regardless of mDNS state; every mDNS
 * start (including reconnects) replays the full cache so TXT records survive
 * wifi flaps without a device reboot. Cache entries are never cleared — only
 * overwritten when the same key is set again. */
#ifdef CONFIG_BB_MDNS_TXT_PENDING_MAX
#define BB_MDNS_TXT_PENDING_MAX CONFIG_BB_MDNS_TXT_PENDING_MAX
#else
#define BB_MDNS_TXT_PENDING_MAX 4
#endif
typedef struct { char key[16]; char value[64]; bool in_use; } bb_mdns_txt_pending_t;
static bb_mdns_txt_pending_t s_txt_pending[BB_MDNS_TXT_PENDING_MAX];

static void txt_pending_store(const char *key, const char *value)
{
    /* Update existing slot if key already pending; otherwise take first free. */
    int free_slot = -1;
    for (int i = 0; i < BB_MDNS_TXT_PENDING_MAX; i++) {
        if (s_txt_pending[i].in_use && strcmp(s_txt_pending[i].key, key) == 0) {
            bb_strlcpy(s_txt_pending[i].value, value, sizeof(s_txt_pending[i].value));
            return;
        }
        if (!s_txt_pending[i].in_use && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        bb_log_w(TAG, "txt pending buffer full, dropping %s=%s", key, value);
        return;
    }
    bb_strlcpy(s_txt_pending[free_slot].key, key, sizeof(s_txt_pending[free_slot].key));
    bb_strlcpy(s_txt_pending[free_slot].value, value, sizeof(s_txt_pending[free_slot].value));
    s_txt_pending[free_slot].in_use = true;
}

static void txt_pending_flush(const char *service_type)
{
    for (int i = 0; i < BB_MDNS_TXT_PENDING_MAX; i++) {
        if (!s_txt_pending[i].in_use) continue;
        esp_err_t err = mdns_service_txt_item_set(service_type, "_tcp",
                                                  s_txt_pending[i].key,
                                                  s_txt_pending[i].value);
        if (err != ESP_OK) {
            bb_log_w(TAG, "txt replay %s=%s failed: %s",
                     s_txt_pending[i].key, s_txt_pending[i].value, esp_err_to_name(err));
        }
        /* Do NOT clear in_use: cache is persistent so the next mDNS start
         * (wifi reconnect) replays the same entries without requiring another
         * set_txt call from the application. */
    }
}

// Find subscription by service and proto; return NULL if not found
/* Compare service/proto strings tolerantly: IDF passes the strings back to
 * the notifier with inconsistent leading-underscore handling depending on
 * the result path. Strip a leading '_' on both sides before comparing so
 * "_taipanminer" registered matches a result reporting "taipanminer" or
 * "_taipanminer". */
static int eq_token(const char *a, const char *b)
{
    if (!a || !b) return 0;
    if (a[0] == '_') a++;
    if (b[0] == '_') b++;
    return strcmp(a, b) == 0;
}

static bb_mdns_browse_sub_t *browse_sub_find(const char *service, const char *proto)
{
    for (int i = 0; i < BB_MDNS_BROWSE_MAX; i++) {
        if (s_subs[i].in_use &&
            eq_token(s_subs[i].service, service) &&
            eq_token(s_subs[i].proto, proto)) {
            return &s_subs[i];
        }
    }
    return NULL;
}

// Find unused slot; return NULL if table full
static bb_mdns_browse_sub_t *browse_sub_alloc(void)
{
    for (int i = 0; i < BB_MDNS_BROWSE_MAX; i++) {
        if (!s_subs[i].in_use) {
            return &s_subs[i];
        }
    }
    return NULL;
}

// Query worker: dequeues async TXT query requests, performs blocking
// mdns_query_txt on this task (not the caller's), fires callback.
static void bb_mdns_query_task(void *arg)
{
    (void)arg;
    bb_mdns_query_req_t req;
    for (;;) {
        if (xQueueReceive(s_query_queue, &req, portMAX_DELAY) != pdTRUE) continue;

        mdns_result_t *results = NULL;
        esp_err_t err = mdns_query_txt(req.instance_name, req.service, req.proto,
                                       req.timeout_ms, &results);

        bb_mdns_query_result_t out = { .err = (err == ESP_OK) ? BB_OK : (bb_err_t)err };
        bb_mdns_txt_t txt_view[8] = {0};

        if (err == ESP_OK && results) {
            if (results->addr) {
                for (mdns_ip_addr_t *a = results->addr; a; a = a->next) {
                    if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                        snprintf(out.id.ip4, sizeof(out.id.ip4), IPSTR,
                                 IP2STR(&a->addr.u_addr.ip4));
                        break;
                    }
                }
            }
            size_t n = results->txt_count > 8 ? 8 : results->txt_count;
            for (size_t i = 0; i < n; i++) {
                txt_view[i].key   = (char *)results->txt[i].key;
                txt_view[i].value = (char *)results->txt[i].value;
            }
            if (results->instance_name) {
                if (strlen(results->instance_name) >= sizeof(out.id.instance_name)) {
                    bb_log_w(TAG, "query: instance_name truncated (src %zu > max %zu)",
                             strlen(results->instance_name), sizeof(out.id.instance_name) - 1);
                }
                bb_strlcpy(out.id.instance_name, results->instance_name, sizeof(out.id.instance_name));
            }
            if (results->hostname) {
                if (strlen(results->hostname) >= sizeof(out.id.hostname)) {
                    bb_log_w(TAG, "query: hostname truncated (src %zu > max %zu)",
                             strlen(results->hostname), sizeof(out.id.hostname) - 1);
                }
                bb_strlcpy(out.id.hostname, results->hostname, sizeof(out.id.hostname));
            }
            out.id.port   = results->port;
            out.txt       = n ? txt_view : NULL;
            out.txt_count = n;
        }

        if (req.cb) req.cb(&out, req.ctx);

        if (results) mdns_query_results_free(results);
    }
}

// Dispatch a single evt entry: resolve ip4 if missing, fire per-peer callbacks.
// Called from the dispatch task for each entry in a batch.
static void dispatch_one(const bb_mdns_evt_t *evt)
{
    bb_mdns_peer_cb on_peer = NULL;
    bb_mdns_peer_removed_cb on_removed = NULL;
    void *ctx = NULL;
    xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
    bb_mdns_browse_sub_t *sub = browse_sub_find(evt->service, evt->proto);
    if (sub) {
        on_peer    = sub->on_peer;
        on_removed = sub->on_removed;
        ctx        = sub->ctx;
    }
    xSemaphoreGive(s_subs_mutex);

    // If add event has no ip4, attempt a short A-record lookup before dispatch.
    // Blocks only the worker task (not the IDF mDNS task) — acceptable on LAN.
    char ip4_resolved[BB_MDNS_IP4_MAX];
    bb_strlcpy(ip4_resolved, evt->ip4, sizeof(ip4_resolved));
    if (!evt->is_removal && ip4_resolved[0] == '\0' && evt->hostname[0] != '\0') {
        esp_ip4_addr_t out_ip;
        esp_err_t qerr = mdns_query_a(evt->hostname, 200, &out_ip);
        if (qerr == ESP_OK) {
            snprintf(ip4_resolved, sizeof(ip4_resolved), IPSTR, IP2STR(&out_ip));
        } else {
            bb_log_d(TAG, "A lookup for %s failed (%s), dispatching without ip4",
                     evt->hostname, esp_err_to_name(qerr));
        }
    }

    if (evt->is_removal) {
        if (on_removed) on_removed(evt->instance_name, ctx);
    } else if (on_peer) {
        bb_mdns_peer_t peer = {0};
        bb_strlcpy(peer.id.instance_name, evt->instance_name, sizeof(peer.id.instance_name));
        bb_strlcpy(peer.id.hostname,      evt->hostname,      sizeof(peer.id.hostname));
        bb_strlcpy(peer.id.ip4,           ip4_resolved,       sizeof(peer.id.ip4));
        peer.id.port   = evt->port;
        peer.txt       = evt->txt_count ? evt->txt : NULL;
        peer.txt_count = evt->txt_count;
        on_peer(&peer, ctx);
    }
}

// Dispatch task: drains the bb_ring dispatch ring and fires consumer callbacks.
// Woken by s_ring_sem; drains ALL items before blocking again so a coalesced
// xSemaphoreGive is never lost.  dispatch_one (knot on_peer_discovered) runs
// OUTSIDE s_evt_pool_lock to prevent lock inversion with the knot layer (B1-372).
//
// Peek/pop uses s_dispatch_item (heap-allocated in bb_mdns_init) rather than a
// stack local.  sizeof(bb_mdns_batch_item_t) at BATCH_MAX=16 is ~8260 B — far
// exceeding this task's 4096 B stack; a stack-local would cause overflow.
static void bb_mdns_dispatch_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_ring_sem, portMAX_DELAY);

        xSemaphoreTake(s_evt_pool_lock, portMAX_DELAY);
        while (bb_ring_count(s_batch_ring) > 0) {
            size_t   out_len = 0;
            int64_t  out_ts  = 0;
            uint32_t out_id  = 0;
            bb_err_t err = bb_ring_peek_oldest(s_batch_ring, s_dispatch_item,
                                               sizeof(*s_dispatch_item),
                                               &out_len, &out_ts, &out_id);
            if (err != BB_OK) {
                /* Unexpected — ring reported count > 0 but peek failed. */
                bb_log_w(TAG, "dispatch: peek_oldest failed (%d), clearing", (int)err);
                bb_ring_clear(s_batch_ring);
                break;
            }
            bb_ring_pop_oldest(s_batch_ring);

            /* Release lock while dispatching — dispatch_one may block (A-record
             * lookup) and the knot on_peer_discovered callback must not run
             * under s_evt_pool_lock (B1-372 lock-order constraint). */
            xSemaphoreGive(s_evt_pool_lock);

            for (int i = 0; i < s_dispatch_item->count; i++) {
                dispatch_one(&s_dispatch_item->entries[i]);
            }

            xSemaphoreTake(s_evt_pool_lock, portMAX_DELAY);
        }
        xSemaphoreGive(s_evt_pool_lock);
    }
}

// Populate a bb_mdns_evt_t from an mdns_result_t.
static void fill_evt_from_result(bb_mdns_evt_t *evt, const mdns_result_t *r)
{
    memset(evt, 0, sizeof(*evt));
    bb_strlcpy(evt->service, r->service_type ? r->service_type : "", sizeof(evt->service));
    bb_strlcpy(evt->proto,   r->proto        ? r->proto        : "", sizeof(evt->proto));
    if (r->instance_name && strlen(r->instance_name) >= sizeof(evt->instance_name)) {
        bb_log_w(TAG, "notifier: instance_name truncated (src %zu > max %zu)",
                 strlen(r->instance_name), sizeof(evt->instance_name) - 1);
    }
    bb_strlcpy(evt->instance_name,
               r->instance_name ? r->instance_name : "",
               sizeof(evt->instance_name));

    if (r->ttl == 0) {
        evt->is_removal = true;
        return;
    }

    evt->is_removal = false;
    if (r->hostname && strlen(r->hostname) >= sizeof(evt->hostname)) {
        bb_log_w(TAG, "notifier: hostname truncated (src %zu > max %zu)",
                 strlen(r->hostname), sizeof(evt->hostname) - 1);
    }
    bb_strlcpy(evt->hostname, r->hostname ? r->hostname : "", sizeof(evt->hostname));
    evt->port = r->port;

    // First IPv4 address
    if (r->addr) {
        for (mdns_ip_addr_t *addr = r->addr; addr; addr = addr->next) {
            if (addr->addr.type == ESP_IPADDR_TYPE_V4) {
                snprintf(evt->ip4, sizeof(evt->ip4), IPSTR,
                         IP2STR(&addr->addr.u_addr.ip4));
                break;
            }
        }
    }

    // TXT records: pack key\0value\0 into payload[], point txt[].key/value in
    size_t n = r->txt_count;
    if (n > BB_MDNS_EVT_TXT_MAX) {
        bb_log_w(TAG, "notifier: %zu txt records exceed cap %d, truncating",
                 n, BB_MDNS_EVT_TXT_MAX);
        n = BB_MDNS_EVT_TXT_MAX;
    }
    char *p   = evt->payload;
    char *end = evt->payload + sizeof(evt->payload);
    for (size_t i = 0; i < n && p < end; i++) {
        const char *k = r->txt[i].key   ? r->txt[i].key   : "";
        const char *v = r->txt[i].value ? r->txt[i].value : "";
        size_t klen = strlen(k);
        size_t vlen = strlen(v);
        if (p + klen + 1 + vlen + 1 > end) {
            bb_log_w(TAG, "notifier: payload full, truncating txt at %zu", i);
            break;
        }
        memcpy(p, k, klen + 1);
        evt->txt[i].key = p;
        p += klen + 1;
        memcpy(p, v, vlen + 1);
        evt->txt[i].value = p;
        p += vlen + 1;
        evt->txt_count++;
    }
}

// Internal notifier called by mdns_browse for all results — runs on IDF mDNS task.
// Coalesces all peers from a single IDF callback burst into the pending batch
// buffer under s_evt_pool_lock.  The flush timer drains the batch as one queue
// item after a 50 ms window, keeping the dispatch queue pressure near-zero
// regardless of how many peers IDF reports per callback.
static void internal_notifier(mdns_result_t *results)
{
    if (!s_evt_pool_lock) return;

    xSemaphoreTake(s_evt_pool_lock, portMAX_DELAY);
    for (mdns_result_t *r = results; r; r = r->next) {
        bb_mdns_evt_t tmp;
        fill_evt_from_result(&tmp, r);
        if (!batch_append_locked(&tmp)) {
            /* Both the batch item slot and the dispatcher queue are full —
             * real overload condition.  The per-burst counter resets each time
             * a drain succeeds (s_evt_drop_count is incremented inside
             * flush_timer_cb for queue-full drops too). */
            s_evt_drop_count++;
            if ((s_evt_drop_count & 0x0F) == 1) {
                bb_log_w(TAG, "notifier: queue+batch full, event dropped (%" PRIu32 " total)",
                         s_evt_drop_count);
            }
        }
    }
    xSemaphoreGive(s_evt_pool_lock);
}

static void mdns_build_hostname(char *out, size_t out_size)
{
    if (s_mdns_hostname_set && s_mdns_hostname[0] != '\0') {
        snprintf(out, out_size, "%s", s_mdns_hostname);
    } else {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(out, out_size, "bsp-device-%02x%02x", mac[4], mac[5]);
    }

    /* mDNS label max 63 chars; guard against out_size < 64 */
    if (out_size > 0) out[out_size - 1] = '\0';
}

static void mdns_build_instance_name(char *out, size_t out_size)
{
    const char *base = s_mdns_instance_name_set ? s_mdns_instance_name : "BSP Device";
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    /* Cap base at (out_size - 6) to leave room for "-xxxx\0".
       Without this, gcc's -Wformat-truncation proves the 64-byte
       s_mdns_instance_name could overflow a 64-byte out buffer. */
    const int base_max = (out_size > 6) ? (int)(out_size - 6) : 0;
    snprintf(out, out_size, "%.*s-%02x%02x", base_max, base, mac[4], mac[5]);
}

static int mdns_init_impl(void)
{
    char hostname[BB_MDNS_HOSTNAME_MAX];
    mdns_build_hostname(hostname, sizeof(hostname));

    // Cache the running hostname for bb_mdns_get_hostname()
    bb_strlcpy(s_running_hostname, hostname, sizeof(s_running_hostname));
    s_running_hostname_valid = true;

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        bb_log_e(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return -1;
    }

    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        bb_log_e(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
        return -1;
    }
    char instance_name[BB_MDNS_INSTANCE_NAME_MAX];
    mdns_build_instance_name(instance_name, sizeof(instance_name));
    err = mdns_instance_name_set(instance_name);
    if (err != ESP_OK) {
        bb_log_e(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(err));
        return -1;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    mdns_txt_item_t txt[] = {
        {"board",   BOARD_NAME},
        {"version", app->version},
        {"mac",     mac_str},
    };

    const char *service_type = s_mdns_service_type_set ? s_mdns_service_type : "_bsp";
    err = mdns_service_add(NULL, service_type, "_tcp", 80, txt, 3);
    if (err != ESP_OK) {
        bb_log_e(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
        return -1;
    }

    txt_pending_flush(service_type);
    announce_timer_ensure_created();

    // Re-arm any existing browse subscriptions after reconnect (mutex held)
    xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
    for (int i = 0; i < BB_MDNS_BROWSE_MAX; i++) {
        if (!s_subs[i].in_use) continue;
        char svc[32], proto[8];
        bb_strlcpy(svc,   s_subs[i].service, sizeof(svc));
        bb_strlcpy(proto, s_subs[i].proto,   sizeof(proto));
        xSemaphoreGive(s_subs_mutex);
        mdns_browse_t *handle = mdns_browse_new(svc, proto, internal_notifier);
        if (!handle) {
            bb_log_w(TAG, "browse re-arm failed: %s.%s", svc, proto);
        } else {
            bb_log_d(TAG, "browse re-armed: %s.%s", svc, proto);
        }
        xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
    }
    xSemaphoreGive(s_subs_mutex);

    bb_log_i(TAG, "mDNS started: %s.local (%s._tcp)", hostname, service_type);
    return 0;
}

static void mdns_free_impl(void)
{
    mdns_service_remove_all();
    mdns_free();
}

static void mdns_send_bye_impl(void)
{
    /* Give the mdns task time to emit bye packets before proceeding. */
    vTaskDelay(pdMS_TO_TICKS(100));
}

static const bb_mdns_lifecycle_adapter_t s_lc_adapter = {
    .mdns_init = mdns_init_impl,
    .mdns_free = mdns_free_impl,
    .mdns_apply_announce = apply_announce_locked,
    .mdns_send_bye = mdns_send_bye_impl,
};

static void bb_mdns_start_internal(void)
{
    bb_mdns_lifecycle_result_t res = bb_mdns_lifecycle_start(&s_lc, &s_lc_adapter);
    switch (res) {
    case BB_MDNS_LC_OK:
        break;
    case BB_MDNS_LC_ALREADY_STARTED:
        bb_log_d(TAG, "bb_mdns_start_internal: already started");
        break;
    case BB_MDNS_LC_INIT_FAILED:
        bb_log_e(TAG, "bb_mdns_start_internal: init failed");
        break;
    case BB_MDNS_LC_NOT_STARTED:
        bb_log_e(TAG, "bb_mdns_start_internal: invalid state");
        break;
    case BB_MDNS_LC_INVALID_ARG:
        bb_log_e(TAG, "bb_mdns_start_internal: invalid arg");
        break;
    }
}

// Start (or restart) mDNS synchronously. Used by consumers that have just
// called bb_mdns_deinit() and want to re-arm without waiting for the next
// wifi got-IP event. Safe to call before bb_mdns_init() — becomes a no-op
// until init has run (guarded by s_lifecycle_mutex check).
void bb_mdns_start(void)
{
    // Guard: if not initialized yet, be a no-op until bb_mdns_init() runs
    if (!s_lifecycle_mutex) return;

    bb_mdns_start_internal();
    if (bb_mdns_lifecycle_is_started(&s_lc)) {
        char instance_name[BB_MDNS_INSTANCE_NAME_MAX];
        mdns_build_instance_name(instance_name, sizeof(instance_name));
        mdns_instance_name_set(instance_name);
        browse_refresh_timer_start();
    }
}

/* Refresh work function: walks s_subs[] and replays mdns_browse_delete +
 * mdns_browse_new for each in-use slot. Runs on bb_timer_disp task (off
 * the esp_timer service task). Snapshots svc/proto under the subs mutex,
 * then drops the lock for the IDF calls (same pattern as
 * bb_mdns_re_arm_browses) to avoid blocking the dispatch task while
 * the IDF mDNS task processes the delete/new. */
static void browse_refresh_work_fn(void *arg)
{
    (void)arg;
    if (!bb_mdns_lifecycle_is_started(&s_lc)) return;

    int refreshed = 0;
    for (int i = 0; i < BB_MDNS_BROWSE_MAX; i++) {
        char svc[32]   = {0};
        char proto[8]  = {0};
        bool in_use = false;
        if (s_subs_mutex && xSemaphoreTake(s_subs_mutex, portMAX_DELAY) == pdTRUE) {
            in_use = s_subs[i].in_use;
            if (in_use) {
                bb_strlcpy(svc,   s_subs[i].service, sizeof(svc));
                bb_strlcpy(proto, s_subs[i].proto,   sizeof(proto));
            }
            xSemaphoreGive(s_subs_mutex);
        }
        if (!in_use) continue;

        /* Best-effort delete; ignore "not found" — a concurrent stop or
         * prior refresh failure would have left the slot bookkeeping out
         * of sync with IDF, and we still want to seed a fresh browser.
         * B1-539: under mDNS action-queue pressure the delete's ENQUEUE
         * can fail (ESP_ERR_NO_MEM) while the existing browse is still
         * live; recreating unconditionally in that case would issue
         * mdns_browse_new against a browse that was never torn down,
         * orphaning the old handle. Map the esp_err_t to the pure
         * decision enum and only recreate when it says it's safe. */
        esp_err_t del_err = mdns_browse_delete(svc, proto);
        bb_mdns_refresh_delete_rc_t del_rc =
            (del_err == ESP_OK)      ? BB_MDNS_REFRESH_DELETE_OK :
            (del_err == ESP_ERR_NO_MEM) ? BB_MDNS_REFRESH_DELETE_NO_MEM :
                                          BB_MDNS_REFRESH_DELETE_OTHER;

        if (!bb_mdns_refresh_should_recreate(del_rc)) {
            s_refresh_skip_count++;
            if ((s_refresh_skip_count & 0x0F) == 1) {
                bb_log_w(TAG,
                         "browse refresh: delete enqueue failed for %s.%s, "
                         "skipping recreate this cycle (%" PRIu32
                         " skip events total)",
                         svc, proto, s_refresh_skip_count);
            }
            continue;
        }

        mdns_browse_t *handle = mdns_browse_new(svc, proto, internal_notifier);
        if (!handle) {
            bb_log_w(TAG, "browse refresh failed: %s.%s", svc, proto);
        } else {
            refreshed++;
        }
    }
    if (refreshed > 0) {
        bb_log_d(TAG, "refreshed %d browse subscription(s)", refreshed);
    }
}

static void browse_refresh_timer_start(void)
{
#if CONFIG_BB_MDNS_BROWSE_REFRESH_INTERVAL_S > 0
    if (s_browse_refresh_timer) return;
    bb_err_t err = bb_timer_deferred_periodic_create(browse_refresh_work_fn, NULL,
                                                      "bb_mdns_browse_refresh",
                                                      &s_browse_refresh_timer);
    if (err != BB_OK) {
        bb_log_w(TAG, "bb_timer_deferred_periodic_create(browse_refresh) failed: %d",
                 (int)err);
        s_browse_refresh_timer = NULL;
        return;
    }
    uint64_t period_us = (uint64_t)CONFIG_BB_MDNS_BROWSE_REFRESH_INTERVAL_S * 1000000ULL;
    err = bb_timer_periodic_start(s_browse_refresh_timer, period_us);
    if (err != BB_OK) {
        bb_log_w(TAG, "bb_timer_periodic_start(browse_refresh) failed: %d", (int)err);
        bb_timer_periodic_delete(s_browse_refresh_timer);
        s_browse_refresh_timer = NULL;
    }
#endif
}

static void browse_refresh_timer_stop(void)
{
    if (!s_browse_refresh_timer) return;
    bb_timer_periodic_stop(s_browse_refresh_timer);
    bb_timer_periodic_delete(s_browse_refresh_timer);
    s_browse_refresh_timer = NULL;
}

static bool mdns_lifecycle_lock(void)
{
    if (!s_lifecycle_mutex) return false;
    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    return true;
}

static void mdns_lifecycle_unlock(void)
{
    if (s_lifecycle_mutex) xSemaphoreGive(s_lifecycle_mutex);
}

static void bb_mdns_on_disconnect(void)
{
    if (!mdns_lifecycle_lock()) return;
    if (!bb_mdns_lifecycle_is_started(&s_lc)) {
        mdns_lifecycle_unlock();
        return;
    }
    bb_log_i(TAG, "wifi disconnected — tearing down mdns");
    if (s_announce_timer) {
        bb_timer_oneshot_stop(s_announce_timer);
        bb_timer_oneshot_delete(s_announce_timer);
        s_announce_timer = NULL;
    }
    browse_refresh_timer_stop();

    bb_mdns_lifecycle_result_t res = bb_mdns_lifecycle_stop(&s_lc, &s_lc_adapter);
    if (res != BB_MDNS_LC_OK && res != BB_MDNS_LC_NOT_STARTED) {
        bb_log_w(TAG, "bb_mdns_on_disconnect: lifecycle stop returned %d", res);
    }
    mdns_lifecycle_unlock();
}

void bb_mdns_deinit(void)
{
    if (!mdns_lifecycle_lock()) return;
    if (!bb_mdns_lifecycle_is_started(&s_lc)) {
        mdns_lifecycle_unlock();
        return;
    }
    if (s_announce_timer) {
        bb_timer_oneshot_stop(s_announce_timer);
        bb_timer_oneshot_delete(s_announce_timer);
        s_announce_timer = NULL;
    }
    browse_refresh_timer_stop();

    bb_mdns_lifecycle_result_t res = bb_mdns_lifecycle_stop(&s_lc, &s_lc_adapter);
    if (res != BB_MDNS_LC_OK && res != BB_MDNS_LC_NOT_STARTED) {
        bb_log_w(TAG, "bb_mdns_deinit: lifecycle stop returned %d", res);
    }
    mdns_lifecycle_unlock();
}

static void bb_mdns_shutdown(void)
{
    bb_mdns_deinit();
}

void bb_mdns_init(void)
{
    // Create mutex and dispatch infrastructure once (outlives WiFi cycles)
    if (!s_subs_mutex) {
        s_subs_mutex = xSemaphoreCreateMutex();
    }
    if (!s_lifecycle_mutex) {
        s_lifecycle_mutex = xSemaphoreCreateMutex();
    }
    if (!s_evt_pool_lock) {
        s_evt_pool_lock = xSemaphoreCreateMutex();
    }
    if (!s_ring_sem) {
        s_ring_sem = xSemaphoreCreateBinary();
        if (!s_ring_sem) {
            bb_log_e(TAG, "xSemaphoreCreateBinary(ring_sem) failed — aborting init");
            return;
        }
    }
    if (!s_batch_ring) {
        bb_err_t err = bb_ring_create(BB_MDNS_BATCH_RING_DEPTH,
                                      sizeof(bb_mdns_batch_item_t),
                                      BB_RING_REJECT_NEW,
                                      "mdns_batch",
                                      &s_batch_ring);
        if (err != BB_OK) {
            bb_log_e(TAG, "bb_ring_create(mdns_batch) failed: %d — aborting init",
                     (int)err);
            return;
        }
    }
    /* Allocate the shared scratch item used by batch_do_flush_locked and the
     * dispatch task.  One allocation; lives for the lifetime of the firmware.
     * Must succeed before the dispatch task or flush timer are created, since
     * both paths dereference s_dispatch_item unconditionally. */
    if (!s_dispatch_item) {
        s_dispatch_item = (bb_mdns_batch_item_t *)bb_calloc_prefer_spiram(1, sizeof(bb_mdns_batch_item_t));
        if (!s_dispatch_item) {
            bb_log_e(TAG, "calloc(dispatch_item) failed — aborting init");
            return;
        }
    }
    if (!s_dispatch_task) {
        bb_task_config_t disp_cfg = {
            .entry       = bb_mdns_dispatch_task,
            .name        = "bb_mdns_disp",
            .arg         = NULL,
            .stack_bytes = 4096,
            .priority    = BB_MDNS_TASK_PRIO,
            .core        = BB_TASK_CORE_ANY,
            .backing     = BB_TASK_BACKING_DYNAMIC,
            .wdt_arm     = false,
        };
        bb_task_create(&disp_cfg, (void **)&s_dispatch_task);
    }
    if (!s_query_queue) {
        s_query_queue = xQueueCreate(BB_MDNS_QUERY_QUEUE_DEPTH, sizeof(bb_mdns_query_req_t));
    }
    if (!s_query_task) {
        bb_task_config_t query_cfg = {
            .entry       = bb_mdns_query_task,
            .name        = "bb_mdns_query",
            .arg         = NULL,
            .stack_bytes = 4096,
            .priority    = BB_MDNS_TASK_PRIO,
            .core        = BB_TASK_CORE_ANY,
            .backing     = BB_TASK_BACKING_DYNAMIC,
            .wdt_arm     = false,
        };
        bb_task_create(&query_cfg, (void **)&s_query_task);
    }

    // Create the coalescing flush timer (one-shot, 50 ms window).
    flush_timer_ensure_created();

    // Register callback with bb_wifi
    bb_wifi_register_on_got_ip(bb_mdns_start);
    bb_wifi_register_on_disconnect(bb_mdns_on_disconnect);
    esp_register_shutdown_handler(bb_mdns_shutdown);
}

void bb_mdns_set_hostname(const char *hostname)
{
    if (!hostname) {
        s_mdns_hostname[0] = '\0';
        s_mdns_hostname_set = false;
        return;
    }
    if (strlen(hostname) >= sizeof(s_mdns_hostname)) {
        bb_log_w(TAG, "set_hostname: truncating (src %zu > max %zu)",
                 strlen(hostname), sizeof(s_mdns_hostname) - 1);
    }
    bb_strlcpy(s_mdns_hostname, hostname, sizeof(s_mdns_hostname));
    s_mdns_hostname_set = true;
}

void bb_mdns_set_service_type(const char *service_type)
{
    if (!service_type) {
        s_mdns_service_type[0] = '\0';
        s_mdns_service_type_set = false;
        return;
    }
    bb_strlcpy(s_mdns_service_type, service_type, sizeof(s_mdns_service_type));
    s_mdns_service_type_set = true;
}

void bb_mdns_set_instance_name(const char *instance_name)
{
    if (!instance_name) {
        s_mdns_instance_name[0] = '\0';
        s_mdns_instance_name_set = false;
        return;
    }
    if (strlen(instance_name) >= sizeof(s_mdns_instance_name)) {
        bb_log_w(TAG, "set_instance_name: truncating (src %zu > max %zu)",
                 strlen(instance_name), sizeof(s_mdns_instance_name) - 1);
    }
    bb_strlcpy(s_mdns_instance_name, instance_name, sizeof(s_mdns_instance_name));
    s_mdns_instance_name_set = true;
}

bool bb_mdns_started(void)
{
    return bb_mdns_lifecycle_is_started(&s_lc);
}

const char *bb_mdns_get_hostname(void)
{
    if (!s_running_hostname_valid || !bb_mdns_lifecycle_is_started(&s_lc)) {
        return NULL;
    }
    return s_running_hostname;
}

void bb_mdns_set_txt(const char *key, const char *value)
{
    if (!key || !value) return;
    /* Always update the persistent cache — it is the source of truth and is
     * replayed on every mDNS start, including wifi-reconnect re-inits. */
    txt_pending_store(key, value);
    if (!bb_mdns_lifecycle_is_started(&s_lc)) {
        return;  /* mDNS not up yet; cache replay on start will apply it */
    }
    /* Write-through: apply immediately to the live mDNS service as well. */
    const char *service_type = s_mdns_service_type_set ? s_mdns_service_type : "_bsp";
    esp_err_t err = mdns_service_txt_item_set(service_type, "_tcp", key, value);
    if (err != ESP_OK) {
        bb_log_w(TAG, "mdns_service_txt_item_set(%s=%s) failed: %s",
                 key, value, esp_err_to_name(err));
    }
    /* Arm coalescing re-announce: observers may miss TXT-only updates without
     * an unsolicited announce. Restart timer on every set_txt so the announce
     * fires after the burst settles (last-write-wins, 100 ms window). */
    announce_arm(BB_MDNS_ANNOUNCE_DELAY_US);
}

void bb_mdns_announce(void)
{
    if (!bb_mdns_lifecycle_is_started(&s_lc)) {
        bb_log_d(TAG, "bb_mdns_announce: service not started, no-op");
        return;
    }
    /* Bypass coalescing delay for explicit calls — fire immediately. */
    announce_arm(0);
}

bb_err_t bb_mdns_browse_start(const char *service, const char *proto,
                              bb_mdns_peer_cb on_peer,
                              bb_mdns_peer_removed_cb on_removed,
                              void *ctx)
{
    if (!service || !proto) {
        return BB_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_subs_mutex, portMAX_DELAY);

    // Check if already subscribed; if so, update callbacks (idempotent)
    bb_mdns_browse_sub_t *existing = browse_sub_find(service, proto);
    if (existing) {
        existing->on_peer    = on_peer;
        existing->on_removed = on_removed;
        existing->ctx        = ctx;
        xSemaphoreGive(s_subs_mutex);
        return BB_OK;
    }

    // Not yet subscribed; find free slot
    bb_mdns_browse_sub_t *slot = browse_sub_alloc();
    if (!slot) {
        xSemaphoreGive(s_subs_mutex);
        return BB_ERR_NO_SPACE;
    }

    // Populate slot
    bb_strlcpy(slot->service, service, sizeof(slot->service));
    bb_strlcpy(slot->proto, proto, sizeof(slot->proto));
    slot->on_peer    = on_peer;
    slot->on_removed = on_removed;
    slot->ctx        = ctx;
    slot->in_use     = true;

    xSemaphoreGive(s_subs_mutex);

    // Start browse outside lock (can be slow)
    mdns_browse_t *handle = mdns_browse_new(service, proto, internal_notifier);
    if (!handle) {
        // Clear slot on failure
        xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
        memset(slot, 0, sizeof(*slot));
        xSemaphoreGive(s_subs_mutex);
        bb_log_e(TAG, "mdns_browse_new(%s.%s) failed", service, proto);
        return BB_ERR_INVALID_STATE;
    }

    bb_log_d(TAG, "browse started: %s.%s", service, proto);
    return BB_OK;
}

bb_err_t bb_mdns_browse_stop(const char *service, const char *proto)
{
    if (!service || !proto) {
        return BB_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
    bb_mdns_browse_sub_t *sub = browse_sub_find(service, proto);
    if (!sub) {
        xSemaphoreGive(s_subs_mutex);
        return BB_OK;  // Already unstarted (idempotent)
    }

    // Clear slot under lock
    memset(sub, 0, sizeof(*sub));
    xSemaphoreGive(s_subs_mutex);

    // Stop browse outside lock
    esp_err_t err = mdns_browse_delete(service, proto);
    if (err != ESP_OK) {
        bb_log_w(TAG, "mdns_browse_delete(%s.%s) failed: %s",
                 service, proto, esp_err_to_name(err));
    }

    bb_log_d(TAG, "browse stopped: %s.%s", service, proto);
    return BB_OK;
}

bb_err_t bb_mdns_query_txt(const char *instance_name, const char *service, const char *proto,
                           uint32_t timeout_ms, bb_mdns_query_cb cb, void *ctx)
{
    if (!instance_name || !service || !proto) return BB_ERR_INVALID_ARG;
    if (!s_query_queue) return BB_ERR_INVALID_STATE;
    bb_mdns_query_req_t req = { .timeout_ms = timeout_ms, .cb = cb, .ctx = ctx };
    bb_strlcpy(req.instance_name, instance_name, sizeof(req.instance_name));
    bb_strlcpy(req.service,       service,       sizeof(req.service));
    bb_strlcpy(req.proto,         proto,         sizeof(req.proto));
    if (xQueueSend(s_query_queue, &req, 0) != pdTRUE) return BB_ERR_NO_SPACE;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Registry auto-registration
// ---------------------------------------------------------------------------

static bb_err_t bb_mdns_registry_init(bb_http_handle_t server)
{
    (void)server;
    bb_mdns_init();
    return BB_OK;
}

#if CONFIG_BB_MDNS_AUTOREGISTER
BB_INIT_REGISTER_N(bb_mdns, bb_mdns_registry_init, 0);
#endif

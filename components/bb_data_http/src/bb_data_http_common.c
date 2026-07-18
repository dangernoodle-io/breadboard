// bb_data_http pure core (B1-1033 PR-2, design KB 1443/1444). See
// bb_data_http.h for the full seam/invariant contract. This file has no
// FreeRTOS, httpd, or bb_data/bb_ws_server dependency -- everything it needs
// beyond bb_queue/bb_core/bb_registry/bb_str/bb_log is reached through the
// three injected seams (render/generation/send).
#include "bb_data_http.h"
#include "bb_data_http_internal.h"

#include "bb_log.h"
#include "bb_registry.h"
#include "bb_str.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "bb_data_http";

// ---------------------------------------------------------------------------
// Kconfig -> C-default bridges. Every symbol here is a plain int Kconfig
// value (never undefined on ESP-IDF -- an int config always generates a
// concrete CONFIG_X), so the bare #ifndef fallback is safe (mirrors
// bb_event_routes_common.c's identical rationale) -- this is NOT the
// boolean-feature-flag shadowing trap the Kconfig-bridge convention warns
// against (see bb_lifecycle_async.c for that pattern).
// ---------------------------------------------------------------------------
#ifndef CONFIG_BB_DATA_HTTP_MAX_CLIENTS
#define CONFIG_BB_DATA_HTTP_MAX_CLIENTS 2
#endif
#ifndef CONFIG_BB_DATA_HTTP_OUTBOUND_CAPACITY
#define CONFIG_BB_DATA_HTTP_OUTBOUND_CAPACITY 8
#endif
#ifndef CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX
#define CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX 512
#endif
#ifndef CONFIG_BB_DATA_HTTP_OUTBOUND_MAX_BYTES
#define CONFIG_BB_DATA_HTTP_OUTBOUND_MAX_BYTES 4096
#endif

// ---------------------------------------------------------------------------
// Attach table (composition-root owned). Name-keyed small lookup table --
// reuses bb_registry rather than hand-rolling a second linear-scan dedupe
// (mirrors bb_data.c's own binding-table shape at the same ~8-entry scale).
// ---------------------------------------------------------------------------

typedef struct {
    bool                        in_use;
    char                        key[BB_DATA_HTTP_KEY_MAX];
    char                        topic[BB_DATA_HTTP_TOPIC_MAX];
    bb_data_http_replay_kind_t  kind;
} attach_slot_t;

static attach_slot_t s_attach[BB_DATA_HTTP_MAX_ATTACH];

BB_REGISTRY_DEFINE_TAGGED(s_attach_registry, BB_DATA_HTTP_MAX_ATTACH, "bb_data_http");

// First slot with in_use == false. Guaranteed to find one whenever
// bb_registry_count(&s_attach_registry) < BB_DATA_HTTP_MAX_ATTACH -- mirrors
// bb_data.c's find_free_slot() exactly (every successful register() here
// claims exactly one slot, neither table supports removal).
static attach_slot_t *find_free_attach_slot(void)
{
    for (size_t i = 0; i < BB_DATA_HTTP_MAX_ATTACH; i++) {
        if (!s_attach[i].in_use) return &s_attach[i];
    }
    return NULL;
}

// The attach-table index used for a key's dirty-mask bit / state_seen_gen
// slot is always the registry's own bb_registry_get_by_index() position
// (register-only, no deregister -- so registration order IS a stable,
// permanent index), never the BSS array position in s_attach[] (which can
// differ once find_free_attach_slot() reuses a hole -- it never does today,
// but nothing below relies on the two coinciding).
bb_err_t bb_data_http_attach_ex(const char *key, const char *topic,
                                bb_data_http_replay_kind_t kind)
{
    if (!key || !key[0] || !topic || !topic[0]) return BB_ERR_INVALID_ARG;
    if (strlen(key) >= BB_DATA_HTTP_KEY_MAX) return BB_ERR_INVALID_ARG;
    if (strlen(topic) >= BB_DATA_HTTP_TOPIC_MAX) return BB_ERR_INVALID_ARG;

    attach_slot_t *slot = (attach_slot_t *)bb_registry_lookup(&s_attach_registry, key);
    if (!slot) {
        slot = find_free_attach_slot();
        if (!slot) return BB_ERR_NO_SPACE;

        bb_strlcpy(slot->key, key, sizeof(slot->key));

        bb_err_t rc = bb_registry_register(&s_attach_registry, slot->key, slot);
        if (rc != BB_OK) return rc;  // LCOV_EXCL_BR_LINE -- find_free_attach_slot()'s in_use scan and the registry's own capacity move in lockstep; cannot actually diverge.
        slot->in_use = true;
    }

    bb_strlcpy(slot->topic, topic, sizeof(slot->topic));
    slot->kind = kind;
    return BB_OK;
}

bb_err_t bb_data_http_attach(const char *key, const char *topic)
{
    return bb_data_http_attach_ex(key, topic, BB_DATA_HTTP_STATE);
}

size_t bb_data_http_attach_count(void)
{
    return bb_registry_count(&s_attach_registry);
}

// True when client `c` should receive updates for a key attached under
// `topic`: an empty topic_filter subscribes to everything; otherwise exact
// match only.
static bool client_subscribes(const bb_data_http_client_t *c, const char *topic)
{
    if (c->topic_filter[0] == '\0') return true;
    return strcmp(c->topic_filter, topic) == 0;
}

// ---------------------------------------------------------------------------
// Injected seams
// ---------------------------------------------------------------------------

static bb_data_http_render_fn     s_render_fn  = NULL;
static void                      *s_render_ctx = NULL;
static bb_data_http_generation_fn s_gen_fn     = NULL;
static void                      *s_gen_ctx    = NULL;
static bb_data_http_send_fn       s_send_fn    = NULL;
static void                      *s_send_ctx   = NULL;

// Cumulative render_fn failure count (bb_data_http_render_fail_count()) plus
// the cadence at which a failure is actually logged -- every failure still
// leaves the dirty bit set for retry (see bb_data_http_sweep_step()), but a
// persistently-failing render_fn must not spam the log on every sweep.
static uint32_t s_render_fail_count;
#define BB_DATA_HTTP_RENDER_FAIL_LOG_EVERY 32

void bb_data_http_set_render_fn(bb_data_http_render_fn fn, void *ctx)
{
    s_render_fn  = fn;
    s_render_ctx = ctx;
}

void bb_data_http_set_generation_fn(bb_data_http_generation_fn fn, void *ctx)
{
    s_gen_fn  = fn;
    s_gen_ctx = ctx;
}

void bb_data_http_set_send_fn(bb_data_http_send_fn fn, void *ctx)
{
    s_send_fn  = fn;
    s_send_ctx = ctx;
}

// ---------------------------------------------------------------------------
// Config / lifecycle
// ---------------------------------------------------------------------------

static struct {
    size_t max_clients;
    bool   initialized;
} s_cfg;

static bb_data_http_client_t s_clients[CONFIG_BB_DATA_HTTP_MAX_CLIENTS];

bb_err_t bb_data_http_init(const bb_data_http_cfg_t *cfg)
{
    if (s_cfg.initialized) return BB_OK;

    s_cfg.max_clients = (cfg && cfg->max_clients) ? cfg->max_clients : CONFIG_BB_DATA_HTTP_MAX_CLIENTS;
    if (s_cfg.max_clients > CONFIG_BB_DATA_HTTP_MAX_CLIENTS) return BB_ERR_INVALID_ARG;

    for (size_t i = 0; i < CONFIG_BB_DATA_HTTP_MAX_CLIENTS; i++) {
        s_clients[i].in_use = false;
    }

    s_cfg.initialized = true;
    bb_log_i(TAG, "initialized: max_clients=%zu", s_cfg.max_clients);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Client lifecycle
// ---------------------------------------------------------------------------

bb_err_t bb_data_http_client_acquire_ex(bb_data_http_client_t **out, int fd,
                                        const char *topic_filter, bool is_ws)
{
    if (!out) return BB_ERR_INVALID_ARG;
    if (!s_cfg.initialized) return BB_ERR_INVALID_STATE;
    // Mirrors bb_data_http_attach_ex()'s own topic-length bound: an
    // over-length filter must be REJECTED, never silently truncated by the
    // bb_strlcpy below (a truncated filter can mis-subscribe a client to a
    // topic it never asked for).
    if (topic_filter && strlen(topic_filter) >= BB_DATA_HTTP_TOPIC_MAX) return BB_ERR_INVALID_ARG;

    for (size_t i = 0; i < s_cfg.max_clients; i++) {
        bb_data_http_client_t *c = &s_clients[i];
        if (c->in_use) continue;

        c->fd     = fd;
        c->is_ws  = is_ws;
        if (topic_filter) {
            bb_strlcpy(c->topic_filter, topic_filter, sizeof(c->topic_filter));
        } else {
            c->topic_filter[0] = '\0';
        }
        c->event_cursor      = 0;
        c->state_dirty_mask  = 0;
        memset(c->state_seen_gen, 0, sizeof(c->state_seen_gen));

        bb_queue_cfg_t qcfg = {
            .capacity_entries = CONFIG_BB_DATA_HTTP_OUTBOUND_CAPACITY,
            .max_entry_bytes  = CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX,
            .policy           = BB_QUEUE_EVICT_OLDEST,
            .name             = "bbdhttp",
            .max_bytes        = CONFIG_BB_DATA_HTTP_OUTBOUND_MAX_BYTES,
            // No age budget: this pure core has no injected clock seam --
            // age-based eviction is left to a future PR if a real cadence
            // needs it. Byte budget alone still bounds worst-case memory.
            .max_age          = 0,
        };
        bb_err_t err = bb_queue_create_ex(&qcfg, &c->outbound);
        if (err != BB_OK) return err;

        // Fresh-render-on-connect: force every subscribed STATE key dirty
        // now, independent of generation comparison. A key that has never
        // been touched (generation still at its initial value) would
        // otherwise never look "dirty" to the generation-diff detect logic
        // in bb_data_http_sweep_step() -- state_seen_gen defaults to 0 here,
        // which can legitimately equal an untouched key's real generation.
        uint16_t count = bb_registry_count(&s_attach_registry);
        for (uint16_t k = 0; k < count; k++) {
            bb_registry_entry_t e;
            if (bb_registry_get_by_index(&s_attach_registry, k, &e) != BB_OK) continue;  // LCOV_EXCL_BR_LINE -- k < count by construction
            attach_slot_t *slot = (attach_slot_t *)e.value;
            if (slot->kind != BB_DATA_HTTP_STATE) continue;
            if (!client_subscribes(c, slot->topic)) continue;
            c->state_dirty_mask |= (1u << k);
        }

        c->in_use = true;
        *out = c;
        return BB_OK;
    }
    return BB_ERR_NO_SPACE;
}

bb_err_t bb_data_http_client_acquire(bb_data_http_client_t **out, int fd, bool is_ws)
{
    return bb_data_http_client_acquire_ex(out, fd, NULL, is_ws);
}

void bb_data_http_client_release(bb_data_http_client_t *c)
{
    if (!c) return;
    bb_queue_destroy(c->outbound);
    c->outbound = NULL;
    c->in_use   = false;
}

size_t bb_data_http_active_client_count(void)
{
    size_t active = 0;
    for (size_t i = 0; i < CONFIG_BB_DATA_HTTP_MAX_CLIENTS; i++) {
        if (s_clients[i].in_use) active++;
    }
    return active;
}

// ---------------------------------------------------------------------------
// Sweep -- see bb_data_http.h's bb_data_http_sweep_step() doc for the full
// two-phase detect/drain contract and the lost-wakeup ordering invariant.
// ---------------------------------------------------------------------------

void bb_data_http_sweep_step(void)
{
    uint16_t attach_count = bb_registry_count(&s_attach_registry);

    // ---- Detect phase: runs to completion for every key/client BEFORE any
    // render call. A generation bump that happens later, during this same
    // pass's render calls (see the drain phase below), is therefore
    // invisible here and gets correctly picked up on the NEXT sweep_step()
    // call instead of being silently absorbed.
    if (s_gen_fn) {
        for (uint16_t k = 0; k < attach_count; k++) {
            bb_registry_entry_t e;
            if (bb_registry_get_by_index(&s_attach_registry, k, &e) != BB_OK) continue;  // LCOV_EXCL_BR_LINE -- k < attach_count by construction
            attach_slot_t *slot = (attach_slot_t *)e.value;
            if (slot->kind != BB_DATA_HTTP_STATE) continue;  // EVENT: TODO PR-3 (shared ring + cursor, B1-1032)

            uint32_t gen = 0;
            if (s_gen_fn(slot->key, &gen, s_gen_ctx) != BB_OK) continue;

            for (size_t i = 0; i < CONFIG_BB_DATA_HTTP_MAX_CLIENTS; i++) {
                bb_data_http_client_t *c = &s_clients[i];
                if (!c->in_use) continue;
                if (!client_subscribes(c, slot->topic)) continue;
                if (c->state_seen_gen[k] == gen) continue;  // unchanged since last render

                c->state_seen_gen[k] = gen;
                c->state_dirty_mask |= (1u << k);
            }
        }
    }

    // ---- Drain phase: per client, for each dirty key, render THEN clear the
    // bit -- and only on render SUCCESS. A missing render_fn has nothing to
    // retry, so it clears unconditionally; a render_fn that fails leaves the
    // bit set so the key is retried next sweep_step() instead of being
    // silently dropped (see bb_data_http_sweep_step()'s doc comment and the
    // render-failure-retry host test). Once every dirty key has been
    // resolved, the outbound queue is flushed via send_fn.
    for (size_t i = 0; i < CONFIG_BB_DATA_HTTP_MAX_CLIENTS; i++) {
        bb_data_http_client_t *c = &s_clients[i];
        if (!c->in_use) continue;
        if (c->state_dirty_mask == 0) continue;

        for (uint16_t k = 0; k < attach_count; k++) {
            uint32_t bit = (1u << k);
            if ((c->state_dirty_mask & bit) == 0) continue;

            if (!s_render_fn) {
                // No render seam installed -- nothing to retry, so degrade
                // gracefully by clearing (see bb_data_http_set_render_fn()).
                c->state_dirty_mask &= ~bit;
                continue;
            }

            bb_registry_entry_t e;
            if (bb_registry_get_by_index(&s_attach_registry, k, &e) != BB_OK) continue;  // LCOV_EXCL_BR_LINE -- k < attach_count by construction
            attach_slot_t *slot = (attach_slot_t *)e.value;

            char     buf[CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX];
            size_t   out_len   = 0;
            bb_err_t render_rc = s_render_fn(slot->key, buf, sizeof(buf), &out_len, s_render_ctx);
            if (render_rc != BB_OK) {
                // Leave the bit set: this key is retried on the next
                // sweep_step() call rather than silently dropped. Rate-limit
                // the log so a persistently-failing render_fn doesn't spam
                // it every sweep, while still making the condition
                // observable via bb_data_http_render_fail_count().
                s_render_fail_count++;
                if (s_render_fail_count == 1 ||
                    (s_render_fail_count % BB_DATA_HTTP_RENDER_FAIL_LOG_EVERY) == 0) {
                    bb_log_w(TAG, "render failed for key '%s': %d (total=%" PRIu32 ")",
                            slot->key, (int)render_rc, s_render_fail_count);
                }
                continue;
            }

            // Clear ONLY on render success -- see the doc comment above.
            c->state_dirty_mask &= ~bit;

            // render_fn's own `cap` argument (sizeof(buf)) always equals the
            // outbound queue's configured max_entry_bytes, so out_len can
            // never exceed it -- push() can only fail here on a genuinely
            // oversized entry, which is unreachable given that invariant.
            bb_err_t push_err = bb_queue_push(c->outbound, buf, out_len, 0, (uint32_t)k);
            if (push_err != BB_OK) {  // LCOV_EXCL_BR_LINE -- unreachable, see comment above
                // LCOV_EXCL_START
                bb_log_w(TAG, "client outbound push failed for key '%s': %d", slot->key, (int)push_err);
                // LCOV_EXCL_STOP
            }
        }

        if (!s_send_fn) continue;

        char    frame[CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX];
        size_t  frame_len = 0;
        int64_t ts        = 0;
        uint32_t id        = 0;
        while (bb_queue_peek_oldest(c->outbound, frame, sizeof(frame), &frame_len, &ts, &id) == BB_OK) {
            s_send_fn(c->fd, c->is_ws, frame, frame_len, s_send_ctx);
            bb_queue_pop_oldest(c->outbound);
        }
    }
}

size_t bb_data_http_render_fail_count(void)
{
    return s_render_fail_count;
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_DATA_HTTP_TESTING
void bb_data_http_reset_for_test(void)
{
    for (size_t i = 0; i < CONFIG_BB_DATA_HTTP_MAX_CLIENTS; i++) {
        if (s_clients[i].in_use) bb_data_http_client_release(&s_clients[i]);
    }
    memset(s_attach, 0, sizeof(s_attach));
    bb_registry_reset(&s_attach_registry);
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_render_fn = NULL; s_render_ctx = NULL;
    s_gen_fn    = NULL; s_gen_ctx    = NULL;
    s_send_fn   = NULL; s_send_ctx   = NULL;
    s_render_fail_count = 0;
}

uint32_t bb_data_http_client_dirty_mask_for_test(const bb_data_http_client_t *c)
{
    return c ? c->state_dirty_mask : 0;
}

uint32_t bb_data_http_client_seen_gen_for_test(const bb_data_http_client_t *c, size_t idx)
{
    if (!c || idx >= BB_DATA_HTTP_MAX_ATTACH) return 0;
    return c->state_seen_gen[idx];
}

size_t bb_data_http_client_outbound_count_for_test(const bb_data_http_client_t *c)
{
    return c ? bb_queue_count(c->outbound) : 0;
}
#endif

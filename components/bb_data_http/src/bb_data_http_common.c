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
#ifndef CONFIG_BB_DATA_HTTP_EVENT_RING_CAPACITY
#define CONFIG_BB_DATA_HTTP_EVENT_RING_CAPACITY 16
#endif
#ifndef CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES
#define CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES 256
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

// Loud guard (B1-1045 PR-4 fix): a binding whose desc->snap_size exceeds the
// shared render scratch (CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES) makes
// bb_data_render() return BB_ERR_NO_SPACE on EVERY sweep, forever -- with
// only a rate-limited WARN (bb_data_http_sweep_step()'s render-fail log).
// That silently starves the key's stream from first render onward (root
// cause of the "log" key never delivering a frame: 220B wire vs a 128B
// scratch). Checking at ATTACH time instead surfaces a misconfiguration at
// wire-up/boot as a loud, unmissable ERROR plus a rejected attach, rather
// than a permanently-failing stream nobody notices until they go looking.
bb_err_t bb_data_http_attach_sized(const char *key, const char *topic,
                                   bb_data_http_replay_kind_t kind,
                                   size_t snap_size)
{
    if (snap_size > (size_t)CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES) {
        bb_log_e(TAG, "attach('%s'): snap_size=%u exceeds render scratch=%u -- "
                "this key would render-fail (BB_ERR_NO_SPACE) every sweep, "
                "forever; not attaching -- bump "
                "CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES",
                key ? key : "(null)", (unsigned)snap_size,
                (unsigned)CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES);
        return BB_ERR_NO_SPACE;
    }
    return bb_data_http_attach_ex(key, topic, kind);
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

// ---------------------------------------------------------------------------
// Shared EVENT ring (B1-1032, KB 1443/1444). A single bb_queue fed by every
// EVENT-kind attached key -- append-only, NOT coalesced (STATE's own
// dirty-mask coalescing does not apply here). Each push carries the
// originating attach-table index in the bb_queue entry's `id` field so a
// client's drain can resolve topic-filter membership.
//
// s_event_total_pushed is the running count of pushes since init/reset --
// NOT current ring occupancy (which shrinks on eviction). A ring entry's own
// global sequence number is derived positionally at drain time rather than
// stored (bb_queue's `id` slot only carries one value, already spent on the
// attach index): the oldest entry currently in the ring has global sequence
// (s_event_total_pushed - bb_queue_count(s_event_ring)).
//
// s_event_last_gen is the module-level (not per-client) last-seen generation
// per attach-table index, used to detect a new EVENT push is needed --
// mirrors STATE's state_seen_gen role but scoped to the whole ring rather
// than any one client, since the ring itself is the single shared consumer
// of generation changes.
static bb_queue_t s_event_ring;
static uint32_t   s_event_total_pushed;
static uint32_t   s_event_last_gen[BB_DATA_HTTP_MAX_ATTACH];

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

    size_t max_clients = (cfg && cfg->max_clients) ? cfg->max_clients : CONFIG_BB_DATA_HTTP_MAX_CLIENTS;
    if (max_clients > CONFIG_BB_DATA_HTTP_MAX_CLIENTS) return BB_ERR_INVALID_ARG;

    size_t ring_capacity = (cfg && cfg->event_ring_capacity) ? cfg->event_ring_capacity
                                                              : CONFIG_BB_DATA_HTTP_EVENT_RING_CAPACITY;
    if (ring_capacity > CONFIG_BB_DATA_HTTP_EVENT_RING_CAPACITY) return BB_ERR_INVALID_ARG;

    bb_queue_cfg_t event_cfg = {
        .capacity_entries = ring_capacity,
        .max_entry_bytes  = CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX,
        .policy           = BB_QUEUE_EVICT_OLDEST,
        .name             = "bbdhevt",
        .max_bytes        = 0,
        .max_age          = 0,
    };
    bb_queue_t event_ring;
    bb_err_t   event_err = bb_queue_create_ex(&event_cfg, &event_ring);
    if (event_err != BB_OK) return event_err;

    // Every validation/allocation step above succeeded -- commit s_cfg (and
    // the rest of the module's live state) only now, never before, so a
    // failed init leaves s_cfg untouched (a caller can retry init() after
    // fixing the cause instead of observing a half-mutated config).
    s_cfg.max_clients     = max_clients;
    s_event_ring          = event_ring;
    s_event_total_pushed  = 0;
    memset(s_event_last_gen, 0, sizeof(s_event_last_gen));

    for (size_t i = 0; i < CONFIG_BB_DATA_HTTP_MAX_CLIENTS; i++) {
        s_clients[i].in_use = false;
    }

    s_cfg.initialized = true;
    bb_log_i(TAG, "initialized: max_clients=%u event_ring_capacity=%u", (unsigned)s_cfg.max_clients, (unsigned)ring_capacity);
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
        // Fresh EVENT clients start at the ring's current head -- they
        // receive only EVENTs pushed AFTER they connect, never ring backlog
        // (unlike STATE's fresh-render-on-connect below; see
        // bb_data_http_client_t's event_cursor doc).
        c->event_cursor              = s_event_total_pushed;
        c->event_dropped             = 0;
        c->event_drop_marker_pending = false;
        c->state_dirty_mask          = 0;
        memset(c->state_seen_gen, 0, sizeof(c->state_seen_gen));

        c->outbound_max_bytes = CONFIG_BB_DATA_HTTP_OUTBOUND_MAX_BYTES;
        bb_queue_cfg_t qcfg = {
            .capacity_entries = CONFIG_BB_DATA_HTTP_OUTBOUND_CAPACITY,
            .max_entry_bytes  = CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX,
            .policy           = BB_QUEUE_EVICT_OLDEST,
            .name             = "bbdhttp",
            .max_bytes        = c->outbound_max_bytes,
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

// True when client `c`'s own outbound queue has room for a `len`-byte push
// WITHOUT relying on outbound's own BB_QUEUE_EVICT_OLDEST auto-eviction. The
// EVENT drain path (unlike STATE's direct bb_queue_push() below) must never
// silently evict an already-queued frame to make room -- that would corrupt
// the append-only ordering guarantee without the client ever finding out
// (see bb_data_http_client_dropped_count()). bb_queue exposes no "would this
// push fit" query, so this checks entry-count AND byte-budget headroom using
// `c`'s own recorded outbound_max_bytes (see bb_data_http_client_t's doc).
static bool client_outbound_has_room(const bb_data_http_client_t *c, size_t len)
{
    if (bb_queue_count(c->outbound) >= bb_queue_capacity(c->outbound)) return false;
    return bb_queue_bytes_used(c->outbound) + len <= c->outbound_max_bytes;
}

// Attempts to flush client `c`'s pending "{"dropped":N}" marker (queued as a
// JSON object, matching the shape of every other frame this queue carries --
// a client that JSON-parses every frame must never see a bare non-JSON
// token) into its outbound queue. A no-op when nothing is pending or when
// outbound has no room yet (the marker then stays pending for a later
// attempt). Called both unconditionally at the top of every
// drain_client_events() sweep -- so a marker set by a PRIOR sweep still
// reaches the wire even if the EVENT feed has since gone quiet and no new
// ring entry ever revisits this client -- and again mid-loop below, so a
// marker that becomes pending partway through THIS sweep still gets queued
// ahead of any surviving entries drained later in the same call.
static void try_flush_event_drop_marker(bb_data_http_client_t *c)
{
    if (!c->event_drop_marker_pending) return;

    char marker[24];
    int  n = snprintf(marker, sizeof(marker), "{\"dropped\":%" PRIu32 "}", c->event_dropped);
    if (n < 0 || (size_t)n >= sizeof(marker)) return;  // LCOV_EXCL_LINE -- uint32_t max always fits this buffer
    if (!client_outbound_has_room(c, (size_t)n)) return;

    bb_queue_push(c->outbound, marker, (size_t)n, 0, 0);
    c->event_drop_marker_pending = false;
}

// Drains every shared-ring EVENT entry newer than `c->event_cursor` -- and
// matching `c`'s topic_filter -- into c's own outbound queue, advancing its
// cursor past each entry it examines (whether delivered, filtered out, or
// dropped for lack of outbound room). See bb_data_http_sweep_step()'s EVENT
// doc for the full contract; this never blocks or otherwise affects any
// other client -- one slow client only ever drops its OWN events.
static void drain_client_events(bb_data_http_client_t *c)
{
    size_t   ring_count = bb_queue_count(s_event_ring);
    uint32_t oldest_seq = s_event_total_pushed - (uint32_t)ring_count;

    // Flush any marker a prior sweep left pending BEFORE anything else this
    // sweep does -- see try_flush_event_drop_marker()'s doc.
    try_flush_event_drop_marker(c);

    // Wraparound-safe: once s_event_total_pushed passes UINT32_MAX, a plain
    // `<` comparison on these uint32_t sequence numbers breaks. The signed-
    // difference idiom (int32_t)(a - b) stays correct across the wrap as
    // long as the true distance between a and b never exceeds INT32_MAX,
    // which holds here (ring/outbound capacities are tiny by comparison).
    if ((int32_t)(c->event_cursor - oldest_seq) < 0) {
        // The ring evicted entries this client never drained -- count the
        // whole gap as dropped and fast-forward the cursor: those sequence
        // numbers no longer exist in the ring to retry.
        c->event_dropped             += (oldest_seq - c->event_cursor);
        c->event_drop_marker_pending  = true;
        c->event_cursor               = oldest_seq;
    }

    for (size_t idx = 0; idx < ring_count; idx++) {
        uint32_t seq = oldest_seq + (uint32_t)idx;
        if ((int32_t)(seq - c->event_cursor) < 0) continue;  // already drained by an earlier sweep (wraparound-safe, see above)

        char     buf[CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX];
        size_t   len = 0;
        int64_t  ts  = 0;
        uint32_t id  = 0;  // attach-table index this entry was pushed under
        if (bb_queue_peek_at(s_event_ring, idx, buf, sizeof(buf), &len, &ts, &id) != BB_OK) continue;  // LCOV_EXCL_BR_LINE -- idx < ring_count by construction

        c->event_cursor = seq + 1;

        bb_registry_entry_t e;
        if (bb_registry_get_by_index(&s_attach_registry, (uint16_t)id, &e) != BB_OK) continue;  // LCOV_EXCL_BR_LINE -- id always names a still-registered attach index (the attach table never shrinks); unreachable in practice.
        attach_slot_t *slot = (attach_slot_t *)e.value;

        if (!client_subscribes(c, slot->topic)) continue;

        try_flush_event_drop_marker(c);

        if (!client_outbound_has_room(c, len)) {
            c->event_dropped            += 1;
            c->event_drop_marker_pending = true;
            continue;
        }

        bb_queue_push(c->outbound, buf, len, ts, id);
    }
}

// ---------------------------------------------------------------------------
// Sweep -- see bb_data_http.h's bb_data_http_sweep_step() doc for the full
// two-phase detect/drain contract and the lost-wakeup ordering invariant.
// ---------------------------------------------------------------------------

void bb_data_http_sweep_step(void)
{
    uint16_t attach_count = bb_registry_count(&s_attach_registry);

    // ---- Detect phase: runs to completion for every key/client BEFORE any
    // STATE render call. A generation bump that happens later, during this
    // same pass's render calls (see the drain phase below), is therefore
    // invisible here and gets correctly picked up on the NEXT sweep_step()
    // call instead of being silently absorbed. EVENT-kind keys are handled
    // inline here too (see the branch below) -- the shared ring has no
    // per-client dirty state to coalesce against, so there is nothing to
    // defer to a later drain phase for the ring-feed step itself (per-client
    // draining of the ring still happens below, in the main per-client
    // loop).
    if (s_gen_fn) {
        for (uint16_t k = 0; k < attach_count; k++) {
            bb_registry_entry_t e;
            if (bb_registry_get_by_index(&s_attach_registry, k, &e) != BB_OK) continue;  // LCOV_EXCL_BR_LINE -- k < attach_count by construction
            attach_slot_t *slot = (attach_slot_t *)e.value;

            uint32_t gen = 0;
            if (s_gen_fn(slot->key, &gen, s_gen_ctx) != BB_OK) continue;

            if (slot->kind == BB_DATA_HTTP_EVENT) {
                if (gen == s_event_last_gen[k]) continue;  // unchanged since last push

                if (!s_render_fn) {
                    // Nothing to retry -- degrade the same way the drain
                    // phase's own no-render_fn path does (see below):
                    // advance past this generation rather than looping on it
                    // forever with no way to ever produce a frame.
                    s_event_last_gen[k] = gen;
                    continue;
                }

                char     ev_buf[CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX];
                size_t   ev_len    = 0;
                bb_err_t render_rc = s_render_fn(slot->key, ev_buf, sizeof(ev_buf), &ev_len, s_render_ctx);
                if (render_rc != BB_OK) {
                    // Leave s_event_last_gen[k] unchanged: retried next
                    // sweep_step() rather than silently skipping the event
                    // -- same clear/advance-only-on-success contract as
                    // STATE's drain phase below, same rate-limited log.
                    s_render_fail_count++;
                    if (s_render_fail_count == 1 ||
                        (s_render_fail_count % BB_DATA_HTTP_RENDER_FAIL_LOG_EVERY) == 0) {
                        bb_log_w(TAG, "event render failed for key '%s': %d (total=%" PRIu32 ")",
                                slot->key, (int)render_rc, s_render_fail_count);
                    }
                    continue;
                }

                bb_queue_push(s_event_ring, ev_buf, ev_len, 0, (uint32_t)k);
                s_event_total_pushed++;
                s_event_last_gen[k] = gen;
                continue;
            }

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

    // ---- Drain phase: per client, for each dirty STATE key, render THEN
    // clear the bit -- and only on render SUCCESS. A missing render_fn has
    // nothing to retry, so it clears unconditionally; a render_fn that fails
    // leaves the bit set so the key is retried next sweep_step() instead of
    // being silently dropped (see bb_data_http_sweep_step()'s doc comment
    // and the render-failure-retry host test). Every active client -- dirty
    // or not -- also has drain_client_events() called on it (EVENT delivery
    // is driven by the shared ring's own state, not this client's
    // state_dirty_mask). Once both are resolved, the outbound queue is
    // flushed via send_fn.
    for (size_t i = 0; i < CONFIG_BB_DATA_HTTP_MAX_CLIENTS; i++) {
        bb_data_http_client_t *c = &s_clients[i];
        if (!c->in_use) continue;

        if (c->state_dirty_mask != 0) {
            for (uint16_t k = 0; k < attach_count; k++) {
                uint32_t bit = (1u << k);
                if ((c->state_dirty_mask & bit) == 0) continue;

                if (!s_render_fn) {
                    // No render seam installed -- nothing to retry, so
                    // degrade gracefully by clearing (see
                    // bb_data_http_set_render_fn()).
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
        }

        drain_client_events(c);

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

uint32_t bb_data_http_client_dropped_count(const bb_data_http_client_t *c)
{
    return c ? c->event_dropped : 0;
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

    bb_queue_destroy(s_event_ring);
    s_event_ring = NULL;
    s_event_total_pushed = 0;
    memset(s_event_last_gen, 0, sizeof(s_event_last_gen));
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

uint32_t bb_data_http_client_event_cursor_for_test(const bb_data_http_client_t *c)
{
    return c ? c->event_cursor : 0;
}
#endif

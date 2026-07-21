// bb_mdns_cache — ESP-IDF glue: bb_mdns browse hello/bye + mdns_query_ptr
// re-query worker, bridging into bb_cache AGE_OUT entries.

#ifdef ESP_PLATFORM

#include "bb_mdns_cache.h"
#include "bb_mdns.h"
#include "bb_cache.h"
#include "bb_cache_internal.h"
#include "bb_json.h"
#include "bb_timer.h"
#include "bb_log.h"
#include "bb_str.h"
#include "mdns.h"
#include "esp_netif.h"
#include "esp_system.h"

#include <string.h>
#include <inttypes.h>

static const char *TAG = "bb_mdns_cache";

// mdns_query_ptr's mdns_result_t.txt is mdns_txt_item_t (const char* fields),
// not the bb_mdns_txt_t (char* fields) apply_txt takes, so the re-query path
// -- unlike on_hello, which already gets a ready-made bb_mdns_txt_t view from
// bb_mdns -- needs an adaptation step. Named per the BB_MDNS_TXT_PENDING_MAX
// idiom (bb_mdns.c) rather than a bare literal; an actual peer TXT set past
// this cap is truncated LOUDLY (bb_log_w below), never silently dropped.
#define BB_MDNS_CACHE_REQUERY_TXT_MAX 8

// bb_mdns_cache_wire_priv.h's BB_MDNS_CACHE_WIRE_TXT_MAX is the SAME cap,
// applied to the "txt" wire array's row count (B1-1115 PR-3) -- the two
// live in different translation units (this file is ESP-IDF-gated; the
// wire descriptor is portable/host-compilable) so they cannot share a
// single #define, but they MUST stay numerically identical or a consumer
// whose TXT set fits the requery path could silently lose rows on the wire.
#include "../../../components/bb_mdns_cache/bb_mdns_cache_wire_priv.h"
_Static_assert(BB_MDNS_CACHE_REQUERY_TXT_MAX == BB_MDNS_CACHE_WIRE_TXT_MAX,
               "BB_MDNS_CACHE_REQUERY_TXT_MAX and BB_MDNS_CACHE_WIRE_TXT_MAX must stay in lockstep");

typedef struct {
    char     service[32];
    char     proto[8];
    char     key_prefix[16];
    uint32_t stale_age_ms;
    uint32_t evict_age_ms;
    uint32_t requery_period_ms;
    size_t   entry_size;
    const bb_mdns_txt_field_t *txt_fields;
    size_t   txt_count;
    bool     started;
} bb_mdns_cache_state_t;

static bb_mdns_cache_state_t s_state = {0};
static bb_periodic_timer_t   s_requery_timer = NULL;

// Effective entry size for this session: cfg->entry_size when set, else the
// identity-only bb_mdns_cache_entry_t shape (today's behavior, byte-
// identical when no consumer descriptor is configured).
static size_t effective_entry_size(void)
{
    return s_state.entry_size ? s_state.entry_size : sizeof(bb_mdns_cache_entry_t);
}

static void entry_serialize(bb_json_t obj, const void *snap)
{
    const bb_mdns_cache_entry_t *e = (const bb_mdns_cache_entry_t *)snap;
    bb_json_obj_set_string(obj, "hostname", e->hostname);
    bb_json_obj_set_string(obj, "ip4", e->ip4);
    bb_json_obj_set_int(obj, "port", (int64_t)e->port);
    if (s_state.txt_fields && s_state.txt_count > 0) {
        bb_mdns_cache_txt_serialize(obj, snap, effective_entry_size(),
                                    s_state.txt_fields, s_state.txt_count);
    }
}

// Register key in bb_cache (AGE_OUT policy), then copy in the entry. Called
// from both the hello handler and the re-query worker -- bb_cache_register()
// is idempotent, so a race between the two contexts registering the same key
// concurrently is harmless. out_first_time reports atomically (single
// s_reg_lock acquisition inside bb_cache) whether THIS call performed the
// key's first-time registration -- closes the TOCTOU a separate
// bb_cache_exists() probe + bb_cache_register() pair would leave open
// between two racing first-time registers of the same key.
static void cache_upsert(const char *key, const void *entry)
{
    bool first_time = false;
    bb_cache_config_t cfg = {
        .key            = key,
        .snapshot       = NULL,
        .snap_size      = effective_entry_size(),
        .serialize      = entry_serialize,
        .flags          = BB_CACHE_FLAG_NONE,
        .eviction       = {
            .policy       = BB_CACHE_EVICT_AGE_OUT,
            .stale_age_ms = s_state.stale_age_ms,
            .evict_age_ms = s_state.evict_age_ms,
        },
        .out_first_time = &first_time,
    };
    bb_err_t err = bb_cache_register(&cfg);
    if (err != BB_OK) {
        bb_log_w(TAG, "bb_cache_register(%s) failed: %d", key, (int)err);
        return;
    }
    if (first_time) {
        bb_log_d(TAG, "peer appeared: %s", key);
    }
    bb_cache_update(&(bb_cache_update_t){ .key = key, .snap = entry });
}

// Installed via bb_cache_set_evict_notify_fn() at bb_mdns_cache_start() --
// bb_mdns_cache is the current installer of that single-slot hook (see
// bb_cache_internal.h's contract comment: exactly one composer at a time,
// never a registry). Fires AFTER free, outside all bb_cache locks, for BOTH
// the LAZY (read-time) floor and the SWEEP backstop -- this is the reliable
// "peer genuinely departed" signal that on_bye below is NOT (see its
// comment): age-out only fires once a peer has stopped being bumped by hello
// or the re-query worker for evict_age_ms, whereas on_bye also fires on
// browse-refresh churn for still-present peers.
static void on_peer_evicted(const char *key)
{
    bb_log_i(TAG, "peer left: %s", key ? key : "(null)");
}

// Light -- no blocking, no mDNS re-entry. Runs on the bb_mdns dispatch task.
static void on_hello(const bb_mdns_peer_t *peer, void *ctx)
{
    (void)ctx;
    // Same gate as the re-query path: a v6-only/unresolved peer can carry an
    // empty ip4, and caching it would make hello and re-query disagree on
    // what counts as a usable entry for the same peer.
    if (!bb_mdns_cache_result_valid(peer->id.instance_name, peer->id.ip4)) return;

    char key[BB_MDNS_CACHE_KEY_MAX];
    bb_err_t err = bb_mdns_cache_build_key(s_state.key_prefix, peer->id.instance_name,
                                           key, sizeof(key));
    if (err != BB_OK && err != BB_ERR_NO_SPACE) return;

    // entry_buf holds effective_entry_size() bytes -- either the plain
    // identity-only shape (default) or the consumer's own struct, which
    // MUST hold identity at this same leading layout (see
    // bb_mdns_cache_config_t.entry_size doc). Bounded by
    // BB_MDNS_CACHE_ENTRY_MAX, validated at bb_mdns_cache_start().
    uint8_t entry_buf[BB_MDNS_CACHE_ENTRY_MAX] = {0};
    bb_mdns_cache_entry_t *identity = (bb_mdns_cache_entry_t *)entry_buf;
    bb_strlcpy(identity->hostname, peer->id.hostname, sizeof(identity->hostname));
    bb_strlcpy(identity->ip4, peer->id.ip4, sizeof(identity->ip4));
    identity->port = peer->id.port;

    if (s_state.txt_fields && s_state.txt_count > 0) {
        bb_mdns_cache_apply_txt(entry_buf, effective_entry_size(),
                                s_state.txt_fields, s_state.txt_count,
                                peer->txt, peer->txt_count);
    }

    cache_upsert(key, entry_buf);
}

// Do NOT delete from the cache here. bb_mdns's browse-refresh workaround
// (CONFIG_BB_MDNS_BROWSE_REFRESH_INTERVAL_S, still active until B1-604) tears
// down and recreates the browse subscription every ~60s and fires this
// callback for STILL-PRESENT peers as documented "normal refresh churn, not a
// real disconnect" (see bb_mdns.h) -- so on_removed is not a reliable
// goodbye. Deleting on it would flicker live peers out of the presence
// cache. Eviction rides AGE_OUT instead: hello + the mdns_query_ptr re-query
// bump ts_ms; a genuinely departed peer stops being bumped and ages out at
// evict_age_ms.
//
// REVISIT after B1-604 removes browse-refresh -- on_removed then becomes a
// clean ttl=0 goodbye that could drive an immediate delete.
static void on_bye(const char *instance_name, void *ctx)
{
    (void)ctx;
    bb_log_d(TAG, "on_bye: %s (refresh churn or real goodbye -- AGE_OUT decides)",
             instance_name ? instance_name : "(null)");
}

// Re-query worker tick: runs on its own dedicated task (bb_timer worker),
// never on the shared bb_timer_disp task -- mdns_query_ptr can block for up
// to CONFIG_BB_MDNS_CACHE_QUERY_TIMEOUT_MS.
//
// This is the benign SSOT refresh: a direct mdns_query_ptr call with no
// browse teardown, so it does not cause the thundering-herd re-subscribe
// bb_mdns's browse-refresh workaround does. It will make bb_mdns's
// browse-refresh (CONFIG_BB_MDNS_BROWSE_REFRESH_INTERVAL_S) redundant once
// B1-604 removes that workaround. Until then, both run harmlessly in
// parallel -- both only ever bump ts_ms via cache_upsert, so this is
// deliberate transitional coexistence, not duplication.
static void requery_work_fn(void *arg)
{
    (void)arg;
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(s_state.service, s_state.proto,
                                   BB_MDNS_CACHE_QUERY_TIMEOUT_MS,
                                   BB_MDNS_CACHE_MAX_RESULTS, &results);
    if (err != ESP_OK) {
        bb_log_w(TAG, "mdns_query_ptr(%s.%s) failed: %s",
                 s_state.service, s_state.proto, esp_err_to_name(err));
        return;
    }

    for (mdns_result_t *r = results; r; r = r->next) {
        char ip4[BB_MDNS_CACHE_IP4_MAX] = {0};
        if (r->addr) {
            for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
                if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                    snprintf(ip4, sizeof(ip4), IPSTR, IP2STR(&a->addr.u_addr.ip4));
                    break;
                }
            }
        }

        if (!bb_mdns_cache_result_valid(r->instance_name, ip4)) continue;

        char key[BB_MDNS_CACHE_KEY_MAX];
        bb_err_t kerr = bb_mdns_cache_build_key(s_state.key_prefix, r->instance_name,
                                                key, sizeof(key));
        if (kerr != BB_OK && kerr != BB_ERR_NO_SPACE) continue;

        // See on_hello: entry_buf holds effective_entry_size() bytes, either
        // the plain identity-only shape or the consumer's own struct with
        // identity at the same leading layout.
        uint8_t entry_buf[BB_MDNS_CACHE_ENTRY_MAX] = {0};
        bb_mdns_cache_entry_t *identity = (bb_mdns_cache_entry_t *)entry_buf;
        bb_strlcpy(identity->hostname, r->hostname ? r->hostname : "", sizeof(identity->hostname));
        bb_strlcpy(identity->ip4, ip4, sizeof(identity->ip4));
        identity->port = r->port;

        if (s_state.txt_fields && s_state.txt_count > 0) {
            if (r->txt_count > BB_MDNS_CACHE_REQUERY_TXT_MAX) {
                bb_log_w(TAG, "requery: %s has %u TXT records, truncating to %d",
                         r->instance_name ? r->instance_name : "(null)",
                         (unsigned)r->txt_count, BB_MDNS_CACHE_REQUERY_TXT_MAX);
            }
            bb_mdns_txt_t txt_view[BB_MDNS_CACHE_REQUERY_TXT_MAX] = {0};
            size_t n = r->txt_count > BB_MDNS_CACHE_REQUERY_TXT_MAX
                           ? BB_MDNS_CACHE_REQUERY_TXT_MAX : r->txt_count;
            for (size_t i = 0; i < n; i++) {
                txt_view[i].key   = (char *)r->txt[i].key;
                txt_view[i].value = (char *)r->txt[i].value;
            }
            bb_mdns_cache_apply_txt(entry_buf, effective_entry_size(),
                                    s_state.txt_fields, s_state.txt_count,
                                    txt_view, n);
        }

        cache_upsert(key, entry_buf);
    }

    mdns_query_results_free(results);
}

bb_err_t bb_mdns_cache_start(const bb_mdns_cache_config_t *cfg)
{
    if (!cfg) return BB_ERR_INVALID_ARG;
    if (!cfg->service) return BB_ERR_INVALID_ARG;
    if (!cfg->proto) return BB_ERR_INVALID_ARG;

    // Both guard checks live in the pure, host-testable
    // bb_mdns_cache_validate_config() -- this call site only decides which
    // log line to emit on failure.
    bb_err_t verr = bb_mdns_cache_validate_config(cfg->entry_size, cfg->txt_fields,
                                                  cfg->txt_count, BB_MDNS_CACHE_ENTRY_MAX);
    if (verr != BB_OK) {
        if (cfg->txt_fields && cfg->txt_count > 0 && cfg->entry_size == 0) {
            bb_log_e(TAG, "bb_mdns_cache_start: txt_fields set but entry_size == 0");
        } else {
            size_t entry_size = cfg->entry_size ? cfg->entry_size : sizeof(bb_mdns_cache_entry_t);
            bb_log_e(TAG, "bb_mdns_cache_start: entry_size %u exceeds BB_MDNS_CACHE_ENTRY_MAX %d",
                     (unsigned)entry_size, BB_MDNS_CACHE_ENTRY_MAX);
        }
        return verr;
    }

    if (s_state.started) {
        bb_log_d(TAG, "bb_mdns_cache_start: already started");
        return BB_OK;
    }

    bb_strlcpy(s_state.service, cfg->service, sizeof(s_state.service));
    bb_strlcpy(s_state.proto, cfg->proto, sizeof(s_state.proto));

    const char *prefix = cfg->key_prefix;
    if (!prefix || prefix[0] == '\0') prefix = BB_MDNS_CACHE_KEY_PREFIX_DEFAULT;
    bb_strlcpy(s_state.key_prefix, prefix, sizeof(s_state.key_prefix));

    s_state.stale_age_ms = cfg->stale_age_ms ? cfg->stale_age_ms : BB_MDNS_CACHE_STALE_AGE_MS;
    s_state.evict_age_ms = cfg->evict_age_ms ? cfg->evict_age_ms : BB_MDNS_CACHE_EVICT_AGE_MS;
    s_state.requery_period_ms = cfg->requery_period_ms ? cfg->requery_period_ms
                                                        : (BB_MDNS_CACHE_REQUERY_PERIOD_S * 1000u);
    s_state.entry_size = cfg->entry_size;
    s_state.txt_fields = cfg->txt_fields;
    s_state.txt_count  = cfg->txt_count;

    // Single-slot install (see bb_cache_internal.h's contract comment) --
    // only reached once, gated by the s_state.started early-return above.
    bb_cache_set_evict_notify_fn(on_peer_evicted);

    bb_err_t err = bb_mdns_browse_start(s_state.service, s_state.proto, on_hello, on_bye, NULL);
    if (err != BB_OK) {
        bb_log_e(TAG, "bb_mdns_browse_start(%s.%s) failed: %d",
                 s_state.service, s_state.proto, (int)err);
        return err;
    }

    if (!s_requery_timer) {
        // BB_MDNS_CACHE_WORKER_STACK (4096) is the bb_timer_worker default,
        // not sized specifically for this worker's mdns_query_ptr internal
        // usage. Validate high-watermark on-device at the CYD soak
        // checkpoint before broad fleet use.
        bb_timer_worker_cfg_t worker_cfg = {
            .stack    = BB_MDNS_CACHE_WORKER_STACK,
            .priority = BB_MDNS_CACHE_WORKER_PRIORITY,
            .core     = -1,
        };
        err = bb_timer_worker_periodic_create(requery_work_fn, NULL, "bb_mdns_cache_rq",
                                              &worker_cfg, &s_requery_timer);
        if (err != BB_OK) {
            bb_log_e(TAG, "bb_timer_worker_periodic_create failed: %d", (int)err);
            bb_mdns_browse_stop(s_state.service, s_state.proto);
            return err;
        }
    }

    err = bb_timer_periodic_start(s_requery_timer, (uint64_t)s_state.requery_period_ms * 1000ULL);
    if (err != BB_OK) {
        bb_log_e(TAG, "bb_timer_periodic_start(requery) failed: %d", (int)err);
        bb_mdns_browse_stop(s_state.service, s_state.proto);
        return err;
    }

    s_state.started = true;
    bb_log_i(TAG, "started: %s.%s, requery every %" PRIu32 " ms",
             s_state.service, s_state.proto, s_state.requery_period_ms);
    return BB_OK;
}

bb_err_t bb_mdns_cache_stop(void)
{
    if (!s_state.started) return BB_OK;

    if (s_requery_timer) {
        bb_timer_periodic_stop(s_requery_timer);
    }
    bb_mdns_browse_stop(s_state.service, s_state.proto);
    s_state.started = false;

    // Release the single-slot evict-notify hook (bb_cache_internal.h's
    // contract) LAST, after hello/bye and the re-query worker are already
    // stopped -- no new AGE_OUT registrations/upserts can land past this
    // point, so on_peer_evicted only has to contend with entries this
    // session already registered. Ordering relative to bb_cache's own sweep
    // worker (BB_CACHE_SWEEP_ENABLE) is a non-issue, not a race we can
    // "win" by reordering: that worker is independent global infrastructure
    // (its own dedicated task, started once by bb_cache_evict_start(), never
    // owned or stopped by bb_mdns_cache) that can run a pass concurrently
    // with this call regardless of where in this function the uninstall
    // happens. bb_cache_set_evict_notify_fn()'s _Atomic-relaxed
    // install-then-fire contract already covers that: a sweep pass
    // in-flight right now reads the OLD (non-NULL) pointer and safely calls
    // on_peer_evicted with a snapshotted key -- never a torn read, never a
    // NULL-deref -- and on_peer_evicted touches no bb_mdns_cache-owned state
    // (it only logs), so a stray post-stop firing is a harmless log line,
    // not a use-after-teardown hazard. A LATER sweep pass (after this store
    // is visible) sees NULL and is a silent no-op (bb_cache_espidf.c's
    // `if (notify) notify(key);` guard). Uninstalling here (rather than
    // leaving it installed indefinitely) is what actually matters: it frees
    // the slot for a future composer, honoring the "current installer, not
    // a permanent owner" contract.
    bb_cache_set_evict_notify_fn(NULL);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Optional self-start (opt-in, CONFIG_BB_MDNS_CACHE_AUTOREGISTER).
// ---------------------------------------------------------------------------

#if defined(CONFIG_BB_MDNS_CACHE_AUTOREGISTER) && CONFIG_BB_MDNS_CACHE_AUTOREGISTER

bb_err_t bb_mdns_cache_init(void)
{
    bb_mdns_cache_config_t cfg = {
        .service = CONFIG_BB_MDNS_CACHE_AUTO_SERVICE,
        .proto   = CONFIG_BB_MDNS_CACHE_AUTO_PROTO,
    };
    return bb_mdns_cache_start(&cfg);
}

#else /* !CONFIG_BB_MDNS_CACHE_AUTOREGISTER: opt-in behavior off, no-op */

bb_err_t bb_mdns_cache_init(void)
{
    return BB_OK;
}

#endif /* CONFIG_BB_MDNS_CACHE_AUTOREGISTER */

#endif /* ESP_PLATFORM */

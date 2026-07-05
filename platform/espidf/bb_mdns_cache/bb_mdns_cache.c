// bb_mdns_cache — ESP-IDF glue: bb_mdns browse hello/bye + mdns_query_ptr
// re-query worker, bridging into bb_cache AGE_OUT entries.

#ifdef ESP_PLATFORM

#include "bb_mdns_cache.h"
#include "bb_mdns.h"
#include "bb_cache.h"
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

typedef struct {
    char     service[32];
    char     proto[8];
    char     key_prefix[16];
    uint32_t stale_age_ms;
    uint32_t evict_age_ms;
    uint32_t requery_period_ms;
    bool     started;
} bb_mdns_cache_state_t;

static bb_mdns_cache_state_t s_state = {0};
static bb_periodic_timer_t   s_requery_timer = NULL;

static void entry_serialize(bb_json_t obj, const void *snap)
{
    const bb_mdns_cache_entry_t *e = (const bb_mdns_cache_entry_t *)snap;
    bb_json_obj_set_string(obj, "hostname", e->hostname);
    bb_json_obj_set_string(obj, "ip4", e->ip4);
    bb_json_obj_set_int(obj, "port", (int64_t)e->port);
}

// Register key in bb_cache (AGE_OUT policy) if not already present, then
// copy in the entry. Called from both the hello handler and the re-query
// worker -- bb_cache_register is idempotent, so a race between the two
// contexts registering the same key concurrently is harmless.
static void cache_upsert(const char *key, const bb_mdns_cache_entry_t *entry)
{
    if (!bb_cache_exists(key)) {
        bb_cache_config_t cfg = {
            .key       = key,
            .snapshot  = NULL,
            .snap_size = sizeof(bb_mdns_cache_entry_t),
            .serialize = entry_serialize,
            .flags     = BB_CACHE_FLAG_NONE,
            .eviction  = {
                .policy       = BB_CACHE_EVICT_AGE_OUT,
                .stale_age_ms = s_state.stale_age_ms,
                .evict_age_ms = s_state.evict_age_ms,
            },
        };
        bb_err_t err = bb_cache_register(&cfg);
        if (err != BB_OK) {
            bb_log_w(TAG, "bb_cache_register(%s) failed: %d", key, (int)err);
            return;
        }
    }
    bb_cache_update(&(bb_cache_update_t){ .key = key, .snap = entry });
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

    bb_mdns_cache_entry_t entry = {0};
    bb_strlcpy(entry.hostname, peer->id.hostname, sizeof(entry.hostname));
    bb_strlcpy(entry.ip4, peer->id.ip4, sizeof(entry.ip4));
    entry.port = peer->id.port;

    cache_upsert(key, &entry);
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

        bb_mdns_cache_entry_t entry = {0};
        bb_strlcpy(entry.hostname, r->hostname ? r->hostname : "", sizeof(entry.hostname));
        bb_strlcpy(entry.ip4, ip4, sizeof(entry.ip4));
        entry.port = r->port;

        cache_upsert(key, &entry);
    }

    mdns_query_results_free(results);
}

bb_err_t bb_mdns_cache_start(const bb_mdns_cache_config_t *cfg)
{
    if (!cfg) return BB_ERR_INVALID_ARG;
    if (!cfg->service) return BB_ERR_INVALID_ARG;
    if (!cfg->proto) return BB_ERR_INVALID_ARG;

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
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Optional self-registration (opt-in, CONFIG_BB_MDNS_CACHE_AUTOREGISTER).
// ---------------------------------------------------------------------------

#if defined(CONFIG_BB_MDNS_CACHE_AUTOREGISTER) && CONFIG_BB_MDNS_CACHE_AUTOREGISTER

#include "bb_init.h"

static bb_err_t bb_mdns_cache_init(void)
{
    bb_mdns_cache_config_t cfg = {
        .service = CONFIG_BB_MDNS_CACHE_AUTO_SERVICE,
        .proto   = CONFIG_BB_MDNS_CACHE_AUTO_PROTO,
    };
    return bb_mdns_cache_start(&cfg);
}

BB_INIT_REGISTER_PRE_HTTP(bb_mdns_cache, bb_mdns_cache_init)

#endif /* CONFIG_BB_MDNS_CACHE_AUTOREGISTER */

#endif /* ESP_PLATFORM */

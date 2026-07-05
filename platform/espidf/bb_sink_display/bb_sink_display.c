// bb_sink_display glue: v1 hand-authored static field catalog + wiring into
// bb_cache_reactive (one observer per distinct selected cache_key), the
// shared coalesced-redraw bb_timer tick, and bb_display's DEFAULT_LINES
// singleton (or the app-owned CUSTOM render_fn). Everything in this file
// composes the pure primitives in
// platform/host/bb_sink_display/bb_sink_display_{select,format,table,validate}.c
// -- no field-selection, formatting, freshness, or config-validation logic
// is duplicated here.
//
// PHASING (v1, locked): s_fields[] below is a hand-authored placeholder
// catalog. A future Lane-3 swap replaces it with a bb_collection-fed
// satellite contribution behind a one-line #if -- bb_sink_display_field_t's
// shape (bb_sink_display.h) does not change.
//
// "Inject the abstraction, not the driver": DEFAULT_LINES calls the
// bb_display singleton's free functions directly (link-time backend
// selection, no runtime handle -- see bb_sink_display.h's header comment).
// CUSTOM policy hands the row subset to cfg->custom and never touches
// bb_display. This file NEVER links LVGL.
//
// Locking / single ownership
// ---------------------------
// s_table is written from two independent tasks: bb_cache_reactive's
// producer/reactive-dispatch task (reactive_on_change/on_register/on_remove,
// via seed_field/seed_key) and the redraw tick (tick_cb, now running on the
// shared bb_timer_disp task via bb_timer_deferred_periodic_create -- see
// the Timer callback convention below). s_table_lock (a FreeRTOS mutex,
// same idiom as bb_sink_ws's s_clients_lock) is taken for every s_table
// read/mutation on BOTH sides so no torn read or dangling row pointer can
// occur. Critical sections are kept tight and NEVER wrap a blocking
// bb_display_*/LVGL call: tick_cb takes the lock only to sweep, collect the
// dirty row *pointers*, and copy their contents into a stack-local
// bb_sink_display_row_t array -- it releases the lock before rendering, so
// render_default_lines()/cfg->custom() only ever touch the local copies,
// never live table storage that a concurrent reactive callback could be
// mutating.
//
// s_selected/s_n_selected/s_caps/s_cfg are write-once during
// bb_sink_display_init() (single-threaded -- no reactive observer or timer
// exists yet) and read-only for the remainder of the process:
// bb_sink_display_start() registers the reactive observers and arms the
// redraw timer strictly after init() returns, so every reader of these
// fields runs after the writes are already complete (ordinary
// call-sequencing happens-before, reinforced by the FreeRTOS
// queue/task-creation calls in between) -- no lock is needed for them.
// This write-once invariant is enforced, not just assumed: s_inited guards
// bb_sink_display_init() against a second call rewriting s_selected/
// s_n_selected/s_caps/s_cfg after start() has armed the observers/timer, and
// s_started guards bb_sink_display_start() against re-registering observers/
// re-arming the timer -- both return BB_ERR_INVALID_STATE on a repeat call.

#include "bb_sink_display.h"

#include "bb_cache.h"
#include "bb_cache_reactive.h"
#include "bb_clock.h"
#include "bb_display.h"
#include "bb_log.h"
#include "bb_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "bb_sink_display";

#ifndef BB_SINK_DISPLAY_ENV_BUF
#define BB_SINK_DISPLAY_ENV_BUF 512
#endif

// ---------------------------------------------------------------------------
// v1 hand-authored static field catalog (Lane-3: swap for bb_collection)
// ---------------------------------------------------------------------------

static const bb_sink_display_field_t s_fields[] = {
    {
        .attrs     = { .priority = 0, .kind = BB_SINK_DISPLAY_KIND_FLOAT,
                       .tag_mask = 0, .delivery_class = BB_ATTRS_DELIVERY_MUST },
        .cache_key = "net.health",
        .json_path = "wifi_rssi_dbm",
        .label     = "RSSI",
        .unit      = "dBm",
        .kind      = BB_SINK_DISPLAY_KIND_FLOAT,
        .format    = NULL,
    },
    {
        .attrs     = { .priority = 1, .kind = BB_SINK_DISPLAY_KIND_INT,
                       .tag_mask = 0, .delivery_class = BB_ATTRS_DELIVERY_MUST },
        .cache_key = "power",
        .json_path = "vout_mv",
        .label     = "Vout",
        .unit      = "mV",
        .kind      = BB_SINK_DISPLAY_KIND_INT,
        .format    = NULL,
    },
    {
        .attrs     = { .priority = 2, .kind = BB_SINK_DISPLAY_KIND_FLOAT,
                       .tag_mask = 0, .delivery_class = BB_ATTRS_DELIVERY_MUST },
        .cache_key = "thermal",
        .json_path = "temp_c",
        .label     = "Temp",
        .unit      = "C",
        .kind      = BB_SINK_DISPLAY_KIND_FLOAT,
        .format    = NULL,
    },
};
#define BB_SINK_DISPLAY_N_FIELDS (sizeof(s_fields) / sizeof(s_fields[0]))

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static bb_sink_display_caps_t         s_caps;
static bb_sink_display_config_t       s_cfg;
static const bb_sink_display_field_t *s_selected[BB_SINK_DISPLAY_MAX_FIELDS];
static size_t                          s_n_selected;
static bb_sink_display_table_t         s_table;
static SemaphoreHandle_t               s_table_lock;   // guards every s_table access
static bb_periodic_timer_t             s_tick;
static bool                            s_inited;
static bool                            s_started;

static inline void table_lock(void)   { xSemaphoreTake(s_table_lock, portMAX_DELAY); }
static inline void table_unlock(void) { xSemaphoreGive(s_table_lock); }

// ---------------------------------------------------------------------------
// Seeding: pull a field's current value out of bb_cache (if already
// registered) without waiting for the next on_change.
// ---------------------------------------------------------------------------

static void seed_field(const bb_sink_display_field_t *field)
{
    char envbuf[BB_SINK_DISPLAY_ENV_BUF];
    size_t env_len = 0;
    if (bb_cache_get_serialized(field->cache_key, envbuf, sizeof(envbuf), &env_len) != BB_OK) {
        return; // key not registered yet (or oversized) -- next on_change/on_register seeds it
    }

    const char *ts_start = NULL, *data_start = NULL;
    size_t ts_len = 0, data_len = 0;
    if (!bb_json_envelope_split(envbuf, (int)env_len, &ts_start, &ts_len, &data_start, &data_len)) {
        bb_log_w(TAG, "seed_field: '%s' envelope split failed", field->cache_key);
        return;
    }

    char valbuf[BB_SINK_DISPLAY_VALUE_MAX];
    if (!bb_sink_display_resolve_field(field, data_start, data_len, valbuf, sizeof(valbuf))) {
        return; // json_path absent in this key's current data
    }

    char tsbuf[24];
    size_t n = ts_len < sizeof(tsbuf) - 1 ? ts_len : sizeof(tsbuf) - 1;
    memcpy(tsbuf, ts_start, n);
    tsbuf[n] = '\0';
    uint64_t ts_ms = (uint64_t)strtoull(tsbuf, NULL, 10);

    table_lock();
    bb_sink_display_table_apply_change(&s_table, field, valbuf, ts_ms);
    table_unlock();
}

static void seed_key(const char *cache_key)
{
    for (size_t i = 0; i < s_n_selected; i++) {
        if (s_selected[i]->cache_key && cache_key &&
            strcmp(s_selected[i]->cache_key, cache_key) == 0) {
            seed_field(s_selected[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// bb_cache_reactive triad
// ---------------------------------------------------------------------------

static void reactive_on_change(const char *key, const char *json, size_t len,
                               int64_t ts_ms, void *ctx)
{
    (void)ctx;
    for (size_t i = 0; i < s_n_selected; i++) {
        const bb_sink_display_field_t *field = s_selected[i];
        if (!field->cache_key || strcmp(field->cache_key, key) != 0) continue;

        char valbuf[BB_SINK_DISPLAY_VALUE_MAX];
        if (!bb_sink_display_resolve_field(field, json, len, valbuf, sizeof(valbuf))) continue;

        table_lock();
        bb_sink_display_table_apply_change(&s_table, field, valbuf, (uint64_t)ts_ms);
        table_unlock();
    }
}

static void reactive_on_register(const char *key, void *ctx)
{
    (void)ctx;
    // v1's field set is fixed at init (hand-authored array) -- rows already
    // exist from bb_sink_display_init()'s table_add pass. "re-select+add"
    // degenerates to seeding the now-available value immediately, instead
    // of waiting for the key's next write to fire on_change.
    seed_key(key);
}

static void reactive_on_remove(const char *key, void *ctx)
{
    (void)ctx;
    table_lock();
    bb_sink_display_table_remove_by_key(&s_table, key);
    table_unlock();
}

// ---------------------------------------------------------------------------
// Coalesced redraw tick: sweep freshness, collect dirty rows, render.
// ---------------------------------------------------------------------------

static void render_default_lines(const bb_sink_display_row_t *rows, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int line = -1;
        for (size_t j = 0; j < s_n_selected; j++) {
            if (s_selected[j] == rows[i].field) { line = (int)j; break; }
        }
        if (line < 0) continue;

        int16_t y = (int16_t)(line * 16); // 8x16 default font line height
        uint16_t fg = rows[i].stale ? 0x7BEFu /* grey565 */ : 0xFFFFu /* white */;
        bb_display_draw_text(0, y, rows[i].value, NULL, fg, 0x0000u);
    }
    bb_display_flush();
}

// Redraw work_fn: runs on the shared bb_timer_disp task (see
// bb_timer_deferred_periodic_create below), never the esp_timer service
// task -- safe to do the (bounded) sweep/collect work here, and to call the
// blocking bb_display_*/LVGL/cfg->custom render path, since neither stalls
// any other esp_timer callback.
static void tick_cb(void *arg)
{
    (void)arg;
    uint64_t now_ms = bb_clock_now_ms64();

    bb_sink_display_row_t local_rows[BB_SINK_DISPLAY_MAX_FIELDS];
    size_t n;

    table_lock();
    bb_sink_display_table_sweep(&s_table, now_ms, s_cfg.stale_after_ms, s_cfg.evict_after_ms);

    const bb_sink_display_row_t *dirty[BB_SINK_DISPLAY_MAX_FIELDS];
    n = bb_sink_display_table_collect_dirty(&s_table, dirty, BB_SINK_DISPLAY_MAX_FIELDS);
    // Copy each dirty row's contents out of live table storage while still
    // holding the lock -- render below runs unlocked (it may block on
    // bb_display_*/LVGL), so it must never dereference a table-owned pointer
    // that a concurrent reactive callback could mutate or (via sweep) shift.
    for (size_t i = 0; i < n; i++) local_rows[i] = *dirty[i];
    table_unlock();

    if (n == 0) return;

    if (s_cfg.kind == BB_SINK_DISPLAY_POLICY_CUSTOM) {
        s_cfg.custom(local_rows, n, s_cfg.ctx);
    } else {
        render_default_lines(local_rows, n);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_sink_display_init(const bb_sink_display_caps_t *caps,
                               const bb_sink_display_config_t *cfg)
{
    if (s_inited) return BB_ERR_INVALID_STATE;

    bb_err_t verr = bb_sink_display_validate_config(caps, cfg);
    if (verr != BB_OK) {
        if (verr == BB_ERR_UNSUPPORTED) {
            bb_log_w(TAG, "cfg->display is reserved; multi-display is not implemented in v1");
        }
        return verr;
    }

    if (!s_table_lock) {
        s_table_lock = xSemaphoreCreateMutex();
        if (!s_table_lock) {
            bb_log_e(TAG, "table lock create failed");
            return BB_ERR_NO_SPACE;
        }
    }

    s_caps = *caps;
    s_cfg  = *cfg;

    if (BB_SINK_DISPLAY_N_FIELDS > BB_SINK_DISPLAY_MAX_FIELDS) {
        bb_log_w(TAG, "catalog has %u fields, truncating to BB_SINK_DISPLAY_MAX_FIELDS=%u",
                 (unsigned)BB_SINK_DISPLAY_N_FIELDS, (unsigned)BB_SINK_DISPLAY_MAX_FIELDS);
    }

    table_lock();
    bb_sink_display_table_init(&s_table);
    s_n_selected = bb_sink_display_select(s_fields, BB_SINK_DISPLAY_N_FIELDS, &s_caps,
                                          s_selected, BB_SINK_DISPLAY_MAX_FIELDS);
    for (size_t i = 0; i < s_n_selected; i++) {
        bb_sink_display_table_add(&s_table, s_selected[i]);
    }
    table_unlock();

    s_inited = true;
    bb_log_i(TAG, "init: selected %u/%u fields (screen_tier=%u max_fields=%u policy=%s)",
             (unsigned)s_n_selected, (unsigned)BB_SINK_DISPLAY_N_FIELDS,
             (unsigned)s_caps.screen_tier, (unsigned)s_caps.max_fields,
             s_cfg.kind == BB_SINK_DISPLAY_POLICY_CUSTOM ? "custom" : "default_lines");
    return BB_OK;
}

bb_err_t bb_sink_display_start(void)
{
    if (!s_inited) return BB_ERR_INVALID_STATE;
    if (s_started) return BB_ERR_INVALID_STATE;
    s_started = true;

    // One observer per distinct selected cache_key -- never observe-all.
    for (size_t i = 0; i < s_n_selected; i++) {
        const char *key = s_selected[i]->cache_key;
        bool dup = false;
        for (size_t j = 0; j < i; j++) {
            if (s_selected[j]->cache_key && key && strcmp(s_selected[j]->cache_key, key) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        bb_cache_reactive_observer_t obs = {
            .key         = key,
            .on_register = reactive_on_register,
            .on_change   = reactive_on_change,
            .on_remove   = reactive_on_remove,
            .ctx         = NULL,
        };
        bb_err_t err = bb_cache_reactive_observe(&obs);
        if (err != BB_OK) {
            bb_log_d(TAG, "reactive observe '%s' unavailable (%d), seeding once only",
                     key, (int)err);
        }
        seed_key(key);
    }

    // Deferred: work_fn (tick_cb) runs on the shared bb_timer_disp task, not
    // the esp_timer service task -- tick_cb makes blocking bb_display_*/
    // cfg->custom calls, which would stall every other esp_timer callback
    // system-wide if run directly on the esp_timer service task (see the
    // Timer callback convention, CLAUDE.md).
    bb_err_t terr = bb_timer_deferred_periodic_create(tick_cb, NULL, "bb_sink_disp", &s_tick);
    if (terr != BB_OK) return terr;
    return bb_timer_periodic_start(s_tick, (uint64_t)s_cfg.rate_limit_ms * 1000ULL);
}

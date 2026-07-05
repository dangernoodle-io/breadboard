// bb_sink_display row table -- pure fixed-capacity bookkeeping. No
// bb_cache_reactive/bb_timer/bb_display calls; bb_cache_evaluate_age()
// (bb_cache.h) is the shared pure freshness classifier already used by
// bb_cache's own AGE_OUT sweep -- reused here rather than hand-rolled, so
// the two-stage ts_ms FSM stays canonical. Shared verbatim between host
// tests and the espidf glue (platform/espidf/bb_sink_display/bb_sink_display.c).

#include "bb_sink_display.h"
#include "bb_cache.h"

#include <string.h>

void bb_sink_display_table_init(bb_sink_display_table_t *t)
{
    if (!t) return;
    memset(t, 0, sizeof(*t));
}

bb_err_t bb_sink_display_table_add(bb_sink_display_table_t *t,
                                    const bb_sink_display_field_t *field)
{
    if (!t || !field) return BB_ERR_INVALID_ARG;

    for (size_t i = 0; i < t->n_entries; i++) {
        if (t->entries[i].row.field == field) return BB_OK; // idempotent
    }
    if (t->n_entries >= BB_SINK_DISPLAY_MAX_FIELDS) return BB_ERR_NO_SPACE;

    bb_sink_display_table_entry_t *e = &t->entries[t->n_entries];
    *e = (bb_sink_display_table_entry_t){0};
    e->row.field = field;
    t->n_entries++;
    return BB_OK;
}

size_t bb_sink_display_table_remove_by_key(bb_sink_display_table_t *t,
                                            const char *cache_key)
{
    if (!t || !cache_key) return 0;

    size_t removed = 0, w = 0;
    for (size_t r = 0; r < t->n_entries; r++) {
        // A populated entry (r < n_entries) always has a non-NULL field --
        // only bb_sink_display_table_add() creates entries, and it rejects
        // a NULL field -- so no defensive f == NULL check is needed here.
        const bb_sink_display_field_t *f = t->entries[r].row.field;
        if (f->cache_key && strcmp(f->cache_key, cache_key) == 0) {
            removed++;
            continue;
        }
        if (w != r) t->entries[w] = t->entries[r];
        w++;
    }
    t->n_entries = w;
    return removed;
}

bb_err_t bb_sink_display_table_apply_change(bb_sink_display_table_t *t,
                                             const bb_sink_display_field_t *field,
                                             const char *value, uint64_t ts_ms)
{
    if (!t || !field || !value) return BB_ERR_INVALID_ARG;

    for (size_t i = 0; i < t->n_entries; i++) {
        if (t->entries[i].row.field != field) continue;

        strncpy(t->entries[i].row.value, value, BB_SINK_DISPLAY_VALUE_MAX - 1);
        t->entries[i].row.value[BB_SINK_DISPLAY_VALUE_MAX - 1] = '\0';
        t->entries[i].last_seen_ms = ts_ms;
        t->entries[i].row.stale    = false;
        t->entries[i].dirty        = true;
        return BB_OK;
    }
    return BB_ERR_NOT_FOUND;
}

size_t bb_sink_display_table_sweep(bb_sink_display_table_t *t, uint64_t now_ms,
                                    uint32_t stale_after_ms, uint32_t evict_after_ms)
{
    if (!t) return 0;

    size_t dropped = 0, w = 0;
    for (size_t r = 0; r < t->n_entries; r++) {
        bb_sink_display_table_entry_t *e = &t->entries[r];
        uint64_t age = (now_ms >= e->last_seen_ms) ? (now_ms - e->last_seen_ms) : 0;
        bb_cache_entry_state_t state = bb_cache_evaluate_age(age, stale_after_ms, evict_after_ms);

        if (state == BB_CACHE_ENTRY_EVICT) {
            dropped++;
            continue; // dropped: not copied forward
        }
        if (state == BB_CACHE_ENTRY_STALE && !e->row.stale) {
            e->row.stale = true;
            e->dirty     = true; // FRESH->STALE transition only, no repeat churn
        }
        if (w != r) t->entries[w] = *e;
        w++;
    }
    t->n_entries = w;
    return dropped;
}

size_t bb_sink_display_table_collect_dirty(bb_sink_display_table_t *t,
                                            const bb_sink_display_row_t **out,
                                            size_t out_cap)
{
    if (!t || !out || out_cap == 0) return 0;

    size_t n = 0;
    for (size_t i = 0; i < t->n_entries && n < out_cap; i++) {
        if (!t->entries[i].dirty) continue;
        out[n++] = &t->entries[i].row;
        t->entries[i].dirty = false;
    }
    return n;
}

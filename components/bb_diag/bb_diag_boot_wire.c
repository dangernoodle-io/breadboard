// bb_diag_boot_wire — the format-agnostic "diag.boot" descriptor SSOT +
// gather. See bb_diag_boot_wire.h for the wire-struct contract. Compiles on
// both host and ESP-IDF; no platform-specific code (bb_cache_get_raw() is
// itself portable).

#include "bb_diag_boot_wire.h"

#include "bb_diag_event_priv.h"
#include "bb_cache.h"
#include "bb_data.h"
#include "bb_str.h"

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Present predicates
// ---------------------------------------------------------------------------

static bool panic_boots_since_present(const void *snap)
{
    const bb_diag_panic_wire_t *p = (const bb_diag_panic_wire_t *)snap;
    return p->available;
}

static bool reboot_detail_present(const void *snap)
{
    const bb_diag_reboot_reason_wire_t *r = (const bb_diag_reboot_reason_wire_t *)snap;
    return r->detail[0] != '\0';
}

static bool reboot_age_s_present(const void *snap)
{
    const bb_diag_reboot_reason_wire_t *r = (const bb_diag_reboot_reason_wire_t *)snap;
    return r->age_s_valid;
}

// ---------------------------------------------------------------------------
// Descriptor
// ---------------------------------------------------------------------------

static const bb_serialize_field_t s_diag_panic_wire_fields[] = {
    { .key = "available", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_diag_panic_wire_t, available) },
    { .key = "boots_since", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_panic_wire_t, boots_since),
      .present = panic_boots_since_present },
};

static const bb_serialize_field_t s_diag_reboot_reason_wire_fields[] = {
    { .key = "source", .type = BB_TYPE_STR,
      .offset = offsetof(bb_diag_reboot_reason_wire_t, source),
      .max_len = sizeof(((bb_diag_reboot_reason_wire_t *)0)->source) },
    { .key = "detail", .type = BB_TYPE_STR,
      .offset = offsetof(bb_diag_reboot_reason_wire_t, detail),
      .max_len = sizeof(((bb_diag_reboot_reason_wire_t *)0)->detail),
      .present = reboot_detail_present },
    { .key = "uptime_s", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_reboot_reason_wire_t, uptime_s) },
    { .key = "epoch_s", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_reboot_reason_wire_t, epoch_s) },
    { .key = "age_s", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_reboot_reason_wire_t, age_s),
      .present = reboot_age_s_present },
};

static const bb_serialize_field_t s_diag_reboot_hist_wire_fields[] = {
    { .key = "source", .type = BB_TYPE_STR,
      .offset = offsetof(bb_diag_reboot_hist_wire_t, source),
      .max_len = sizeof(((bb_diag_reboot_hist_wire_t *)0)->source) },
    { .key = "epoch_s", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_reboot_hist_wire_t, epoch_s) },
    { .key = "uptime_s", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_reboot_hist_wire_t, uptime_s) },
};

static const bb_serialize_field_t s_diag_boot_wire_fields[] = {
    { .key = "reset_reason", .type = BB_TYPE_STR,
      .offset = offsetof(bb_diag_boot_wire_t, reset_reason),
      .max_len = sizeof(((bb_diag_boot_wire_t *)0)->reset_reason) },
    { .key = "wdt_resets", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_boot_wire_t, wdt_resets) },
    { .key = "panic", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_diag_boot_wire_t, panic),
      .children = s_diag_panic_wire_fields,
      .n_children = sizeof(s_diag_panic_wire_fields) / sizeof(s_diag_panic_wire_fields[0]) },
    { .key = "pending_verify", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_diag_boot_wire_t, pending_verify) },
    { .key = "rolled_back", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_diag_boot_wire_t, rolled_back) },
    { .key = "reboot_reason", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_diag_boot_wire_t, reboot_reason),
      .children = s_diag_reboot_reason_wire_fields,
      .n_children = sizeof(s_diag_reboot_reason_wire_fields) / sizeof(s_diag_reboot_reason_wire_fields[0]) },
    { .key = "reboot_history", .type = BB_TYPE_ARR,
      .offset = offsetof(bb_diag_boot_wire_t, reboot_history),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(bb_diag_reboot_hist_wire_t),
      .max_items = BB_REBOOT_HISTORY_CAP,
      .children = s_diag_reboot_hist_wire_fields,
      .n_children = sizeof(s_diag_reboot_hist_wire_fields) / sizeof(s_diag_reboot_hist_wire_fields[0]) },
};

const bb_serialize_desc_t bb_diag_boot_wire_desc = {
    .type_name = "diag_boot",
    .fields    = s_diag_boot_wire_fields,
    .n_fields  = sizeof(s_diag_boot_wire_fields) / sizeof(s_diag_boot_wire_fields[0]),
    .snap_size = sizeof(bb_diag_boot_wire_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1059 PR-2b-i-2, `.required` corrected +
// re-targeted at the REST envelope B1-1189) -- co-located JSON Schema
// companion to bb_diag_boot_wire_desc above, gated behind
// BB_SERIALIZE_META_HOST (see bb_diag_boot_wire.h's banner). Three levels of
// nesting (flat top-level fields, two BB_TYPE_OBJ fields ("panic",
// "reboot_reason"), one BB_TYPE_ARR-of-BB_TYPE_OBJ field ("reboot_history"))
// each need their own co-located sub-table, mirroring
// bb_ota_validator_partitions_wire.c's ARR-of-OBJ precedent (B1-1059
// PR-2b-i-1).
//
// `.required` mirrors bb_diag_boot_wire_desc's own `.present` gating at
// every level: a field with no `.present` predicate is unconditionally
// emitted (required = true); a present-gated field ("boots_since",
// "detail", "age_s") is optional. This is the derivation target for GET
// /api/diag/boot's REST envelope response schema
// (platform/espidf/bb_diag_http/bb_diag_http_routes.c's hand-authored
// s_boot_get_responses[] -- see test_bb_diag_boot_wire_envelope_meta_golden.c),
// which DOES carry a "required" list on both nested BB_TYPE_OBJ fields
// ("panic", "reboot_reason") matching this table's `.required` flags
// exactly (mod the composer's own trailing "additionalProperties":false,
// documented there).
//
// Collateral note for the SAME table's OTHER consumer,
// test_bb_diag_boot_wire_meta_golden.c (the "diag.boot" SSE topic schema,
// k_diag_boot_schema): that hand literal never carries a "required" key on
// either nested object at all, regardless of content, so populating
// `.required` here (needed for the REST envelope above) makes that other
// golden's accepted delta wider (any nested "required" list vs none) but
// not a new CLASS of delta -- see that file's own banner.
//
// "reboot_history"'s ARR-of-OBJ row sub-table also marks every child
// `.required = true` (none of its fields are present-gated), but the
// composer's BB_TYPE_ARR-of-BB_TYPE_OBJ items branch never emits a
// "required" list at all (same engine limitation as the partitions-wire
// precedent) -- an accepted delta, not a content mismatch.
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_SHIP)

static const bb_serialize_field_meta_t s_diag_panic_wire_meta_rows[] = {
    { .key = "available", .required = true },
    { .key = "boots_since" },  // present-gated (panic_boots_since_present) -- optional
};

static const bb_serialize_field_meta_t s_diag_reboot_reason_wire_meta_rows[] = {
    { .key = "source", .required = true },
    { .key = "detail" },  // present-gated (reboot_detail_present) -- optional
    { .key = "uptime_s", .required = true },
    { .key = "epoch_s", .required = true },
    { .key = "age_s" },  // present-gated (reboot_age_s_present) -- optional
};

static const bb_serialize_field_meta_t s_diag_reboot_hist_wire_meta_rows[] = {
    { .key = "source", .required = true },
    { .key = "epoch_s", .required = true },
    { .key = "uptime_s", .required = true },
};

static const bb_serialize_field_meta_t s_diag_boot_wire_meta_rows[] = {
    { .key = "reset_reason",    .required = true },
    { .key = "wdt_resets",      .required = true },
    { .key = "panic",           .required = true,
      .children = s_diag_panic_wire_meta_rows,
      .n_children = sizeof(s_diag_panic_wire_meta_rows) / sizeof(s_diag_panic_wire_meta_rows[0]) },
    { .key = "pending_verify",  .required = true },
    { .key = "rolled_back",     .required = true },
    { .key = "reboot_reason",   .required = true,
      .children = s_diag_reboot_reason_wire_meta_rows,
      .n_children = sizeof(s_diag_reboot_reason_wire_meta_rows) / sizeof(s_diag_reboot_reason_wire_meta_rows[0]) },
    { .key = "reboot_history",  .required = true,
      .children = s_diag_reboot_hist_wire_meta_rows,
      .n_children = sizeof(s_diag_reboot_hist_wire_meta_rows) / sizeof(s_diag_reboot_hist_wire_meta_rows[0]) },
};

const bb_serialize_desc_meta_t bb_diag_boot_wire_meta = {
    .type_name = "diag_boot",
    .rows      = s_diag_boot_wire_meta_rows,
    .n_rows    = sizeof(s_diag_boot_wire_meta_rows) / sizeof(s_diag_boot_wire_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */

// ---------------------------------------------------------------------------
// Gather
// ---------------------------------------------------------------------------

bb_err_t bb_diag_boot_gather(bb_diag_boot_wire_t *dst)
{
    if (!dst) return BB_ERR_INVALID_ARG;

    bb_diag_boot_snap_t raw;
    memset(&raw, 0, sizeof(raw));
    bb_err_t err = bb_cache_get_raw(BB_DIAG_BOOT_TOPIC, &raw, sizeof(raw));
    if (err != BB_OK) return err;

    memset(dst, 0, sizeof(*dst));

    bb_strlcpy(dst->reset_reason, raw.reset_reason, sizeof(dst->reset_reason));
    dst->wdt_resets = (int64_t)raw.wdt_resets;

    dst->panic.available   = raw.panic_available;
    dst->panic.boots_since = (int64_t)raw.panic_boots_since;

    dst->pending_verify = raw.pending_verify;
    dst->rolled_back    = raw.rolled_back;

    bb_strlcpy(dst->reboot_reason.source,
               bb_reset_source_str((bb_reset_source_t)raw.reboot_src),
               sizeof(dst->reboot_reason.source));
    bb_strlcpy(dst->reboot_reason.detail, raw.reboot_detail, sizeof(dst->reboot_reason.detail));
    dst->reboot_reason.uptime_s = (int64_t)raw.reboot_uptime_s;
    dst->reboot_reason.epoch_s  = (int64_t)raw.reboot_epoch_s;
    // Same 3-way guard the now-retired bb_json bb_cache serializer used for
    // its age_s branch: both the recorded epoch and the current wall clock
    // must be known-good, or a not-yet-synced "now" would otherwise produce
    // a bogus (huge or negative) age.
    dst->reboot_reason.age_s_valid = raw.reboot_epoch_s > 0 && raw.now_epoch_valid &&
                                      raw.now_epoch_s >= raw.reboot_epoch_s;
    dst->reboot_reason.age_s = dst->reboot_reason.age_s_valid
        ? (int64_t)(raw.now_epoch_s - raw.reboot_epoch_s) : 0;

    // Materialize the ring newest-first -- same modular-index walk the
    // now-retired bb_json bb_cache serializer used (B1-1053 PR1 deleted it).
    uint8_t n = raw.reboot_history.count;
    if (n > BB_REBOOT_HISTORY_CAP) n = BB_REBOOT_HISTORY_CAP;
    for (uint8_t i = 0; i < n; i++) {
        uint8_t idx = (uint8_t)((raw.reboot_history.head + (n - 1U - i)) % BB_REBOOT_HISTORY_CAP);
        const bb_reboot_hist_entry_t *e = &raw.reboot_history.entries[idx];
        bb_strlcpy(dst->reboot_history_items[i].source,
                   bb_reset_source_str((bb_reset_source_t)e->src),
                   sizeof(dst->reboot_history_items[i].source));
        dst->reboot_history_items[i].epoch_s  = (int64_t)e->epoch_s;
        dst->reboot_history_items[i].uptime_s = (int64_t)e->uptime_s;
    }
    dst->reboot_history.items = dst->reboot_history_items;
    dst->reboot_history.count = n;

    return BB_OK;
}

// ---------------------------------------------------------------------------
// bb_data bind + REST render (B1-1053 PR1) -- see the file-header note in
// bb_diag_boot_wire.h for why the bind lives here.
// ---------------------------------------------------------------------------

// Adapter: bb_data_gather_fn wraps bb_diag_boot_gather()'s typed signature.
// "diag.boot" has no request-scoped filter, so `args` is otherwise unused.
static bb_err_t diag_boot_data_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    return bb_diag_boot_gather((bb_diag_boot_wire_t *)dst);
}

bb_err_t bb_diag_boot_bind(void)
{
    bb_data_binding_t binding = {
        .key    = BB_DIAG_BOOT_TOPIC,
        .desc   = &bb_diag_boot_wire_desc,
        .gather = diag_boot_data_gather,
    };
    return bb_data_bind(&binding);
}

// bb_diag_boot_render_envelope() (the REST GET /api/diag/boot render seam)
// relocated to components/bb_diag_http/bb_diag_http_boot_wire.c (B1-1153,
// KB 1477) -- its sole reason for including bb_http_server.h here was that
// one function; bb_diag itself is bb_http_server-free after this split. See
// bb_diag_http.h for its prototype/doc.

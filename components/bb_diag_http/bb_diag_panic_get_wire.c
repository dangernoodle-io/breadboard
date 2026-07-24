// bb_diag_panic_get_wire — wire descriptor + fill for GET /api/diag/panic.
// See bb_diag_panic_get_wire_priv.h for the full shape/present-condition
// table this migration produces.

#include "bb_diag_panic_get_wire_priv.h"

#include "bb_str.h"

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Present predicates
// ---------------------------------------------------------------------------

static bool boots_since_present(const void *snap)
{
    const bb_diag_panic_get_wire_t *p = (const bb_diag_panic_get_wire_t *)snap;
    return p->available || p->coredump_avail;
}

static bool reset_reason_present(const void *snap)
{
    const bb_diag_panic_get_wire_t *p = (const bb_diag_panic_get_wire_t *)snap;
    return p->available;
}

static bool log_tail_field_present(const void *snap)
{
    const bb_diag_panic_get_wire_t *p = (const bb_diag_panic_get_wire_t *)snap;
    return p->log_tail_present;
}

static bool coredump_fields_present(const void *snap)
{
    const bb_diag_panic_get_wire_t *p = (const bb_diag_panic_get_wire_t *)snap;
    return p->coredump_fields_present;
}

static bool panic_reason_field_present(const void *snap)
{
    const bb_diag_panic_get_wire_t *p = (const bb_diag_panic_get_wire_t *)snap;
    return p->panic_reason_present;
}

static bool app_sha256_field_present(const void *snap)
{
    const bb_diag_panic_get_wire_t *p = (const bb_diag_panic_get_wire_t *)snap;
    return p->app_sha256_present;
}

// ---------------------------------------------------------------------------
// Descriptor
// ---------------------------------------------------------------------------

static const bb_serialize_field_t s_diag_panic_get_wire_fields[] = {
    { .key = "available", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_diag_panic_get_wire_t, available) },
    { .key = "boots_since", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_panic_get_wire_t, boots_since),
      .present = boots_since_present },
    { .key = "reset_reason", .type = BB_TYPE_STR,
      .offset = offsetof(bb_diag_panic_get_wire_t, reset_reason),
      .max_len = sizeof(((bb_diag_panic_get_wire_t *)0)->reset_reason),
      .present = reset_reason_present },
    { .key = "log_tail", .type = BB_TYPE_STR,
      .offset = offsetof(bb_diag_panic_get_wire_t, log_tail),
      .max_len = sizeof(((bb_diag_panic_get_wire_t *)0)->log_tail),
      .present = log_tail_field_present },
    { .key = "task", .type = BB_TYPE_STR,
      .offset = offsetof(bb_diag_panic_get_wire_t, task),
      .max_len = sizeof(((bb_diag_panic_get_wire_t *)0)->task),
      .present = coredump_fields_present },
    { .key = "exc_pc", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_panic_get_wire_t, exc_pc),
      .present = coredump_fields_present },
    { .key = "exc_cause", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_panic_get_wire_t, exc_cause),
      .present = coredump_fields_present },
    { .key = "backtrace", .type = BB_TYPE_ARR,
      .offset = offsetof(bb_diag_panic_get_wire_t, backtrace),
      .elem_type = BB_TYPE_I64,
      .max_items = BB_DIAG_PANIC_BACKTRACE_MAX,
      .present = coredump_fields_present },
    { .key = "panic_reason", .type = BB_TYPE_STR,
      .offset = offsetof(bb_diag_panic_get_wire_t, panic_reason),
      .max_len = sizeof(((bb_diag_panic_get_wire_t *)0)->panic_reason),
      .present = panic_reason_field_present },
    { .key = "app_sha256", .type = BB_TYPE_STR,
      .offset = offsetof(bb_diag_panic_get_wire_t, app_sha256),
      .max_len = sizeof(((bb_diag_panic_get_wire_t *)0)->app_sha256),
      .present = app_sha256_field_present },
};

const bb_serialize_desc_t bb_diag_panic_get_wire_desc = {
    .type_name = "bb_diag_panic_get_wire_t",
    .fields    = s_diag_panic_get_wire_fields,
    .n_fields  = sizeof(s_diag_panic_get_wire_fields) / sizeof(s_diag_panic_get_wire_fields[0]),
    .snap_size = sizeof(bb_diag_panic_get_wire_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1059 PR-3a meta-derivation feeder) --
// co-located JSON Schema companion to bb_diag_panic_get_wire_desc above,
// gated behind BB_SERIALIZE_META_HOST (see bb_diag_panic_get_wire_priv.h's
// banner). "required" mirrors platform/espidf/bb_diag_http/
// bb_diag_http_routes.c's hand-authored s_panic_get_responses[] 200
// literal, which lists ONLY "available" as required -- every other field
// is present-gated at runtime, matching this table's non-required rows.
// See test_bb_diag_panic_get_wire_meta_golden.c for the fidelity proof.
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_SHIP)

static const bb_serialize_field_meta_t s_diag_panic_get_wire_meta_rows[] = {
    { .key = "available",    .required = true },
    { .key = "boots_since" },    // present-gated (boots_since_present) -- optional
    { .key = "reset_reason" },   // present-gated (reset_reason_present) -- optional
    { .key = "log_tail" },       // present-gated (log_tail_field_present) -- optional
    { .key = "task" },           // present-gated (coredump_fields_present) -- optional
    { .key = "exc_pc" },         // present-gated (coredump_fields_present) -- optional
    { .key = "exc_cause" },      // present-gated (coredump_fields_present) -- optional
    { .key = "backtrace" },      // present-gated (coredump_fields_present) -- optional
    { .key = "panic_reason" },   // present-gated (panic_reason_field_present) -- optional
    { .key = "app_sha256" },     // present-gated (app_sha256_field_present) -- optional
};

const bb_serialize_desc_meta_t bb_diag_panic_get_wire_meta = {
    .type_name = "bb_diag_panic_get_wire_t",
    .rows      = s_diag_panic_get_wire_meta_rows,
    .n_rows    = sizeof(s_diag_panic_get_wire_meta_rows) / sizeof(s_diag_panic_get_wire_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */

// ---------------------------------------------------------------------------
// Fill
// ---------------------------------------------------------------------------

void bb_diag_panic_get_wire_fill(bb_diag_panic_get_wire_t *dst,
                                  bool available, bool coredump_avail,
                                  uint32_t boots_since, const char *reset_reason,
                                  bool log_tail_ok, const char *log_tail,
                                  const bb_diag_panic_summary_t *summary)
{
    memset(dst, 0, sizeof(*dst));

    dst->available      = available;
    dst->coredump_avail = coredump_avail;
    dst->boots_since     = (int64_t)boots_since;

    if (reset_reason) {
        bb_strlcpy(dst->reset_reason, reset_reason, sizeof(dst->reset_reason));
    }

    dst->log_tail_present = available && log_tail_ok;
    if (dst->log_tail_present && log_tail) {
        bb_strlcpy(dst->log_tail, log_tail, sizeof(dst->log_tail));
    }

    dst->backtrace.items = dst->backtrace_items;
    dst->backtrace.count = 0;

    // Distinct from `coredump_avail` above: this is the "the get() call
    // also succeeded" predicate that gates task/exc_pc/exc_cause/backtrace
    // (and, further, panic_reason/app_sha256) -- byte-fidelity with the
    // pre-migration handler, which nested those fields inside a successful
    // bb_diag_panic_coredump_get() call, not merely a true coredump_avail.
    dst->coredump_fields_present = coredump_avail && (summary != NULL);

    if (coredump_avail && summary) {
        bb_strlcpy(dst->task, summary->task_name, sizeof(dst->task));
        dst->exc_pc    = (int64_t)summary->exc_pc;
        dst->exc_cause = (int64_t)summary->exc_cause;

        uint32_t n = summary->bt_count;
        if (n > BB_DIAG_PANIC_BACKTRACE_MAX) n = BB_DIAG_PANIC_BACKTRACE_MAX;
        for (uint32_t i = 0; i < n; i++) {
            dst->backtrace_items[i] = (int64_t)summary->bt_addrs[i];
        }
        dst->backtrace.count = n;

        dst->panic_reason_present = summary->panic_reason[0] != '\0';
        if (dst->panic_reason_present) {
            bb_strlcpy(dst->panic_reason, summary->panic_reason, sizeof(dst->panic_reason));
        }

        dst->app_sha256_present = summary->app_sha256[0] != '\0';
        if (dst->app_sha256_present) {
            bb_strlcpy(dst->app_sha256, summary->app_sha256, sizeof(dst->app_sha256));
        }
    }
}

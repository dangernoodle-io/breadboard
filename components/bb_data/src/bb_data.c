// bb_data core binding table (B1-832). See bb_data.h for the seam contract.
// Stores the key -> (desc, gather) binding table in a bb_registry instance
// (name-keyed, one bb_data slot's address per entry) -- no hand-rolled
// fixed array + linear string scan.
#include "bb_data.h"

#include "bb_log.h"
#include "bb_registry.h"
#include "bb_serialize_format.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "bb_data";

// One key's stored binding -- a BY-VALUE copy of the caller's
// bb_data_binding_t (the caller's own struct may be a stack temporary), plus
// an owned copy of the key string (a stable, static-storage name pointer for
// the bb_registry entry -- the registry only borrows `name`, it never copies
// it).
typedef struct {
    bool                          in_use;
    char                          key[BB_DATA_KEY_MAX];
    const bb_serialize_desc_t    *desc;
    bb_data_gather_fn             gather;
    bb_data_apply_fn              apply;
    void                         *ctx;
    bb_data_replay_kind_t         replay_kind;
    const bb_data_row_binding_t  *rows;
    _Atomic uint32_t              generation;
} bb_data_slot_t;

static bb_data_slot_t s_slots[BB_DATA_MAX_BINDINGS];

BB_REGISTRY_DEFINE_TAGGED(s_bb_data_registry, BB_DATA_MAX_BINDINGS, "bb_data");

// First slot with in_use == false. Guaranteed to find one whenever
// bb_registry_count(&s_bb_data_registry) < BB_DATA_MAX_BINDINGS, since every
// successful bb_registry_register() call below is paired 1:1 with claiming
// exactly one slot here (and neither table supports removal).
static bb_data_slot_t *find_free_slot(void)
{
    for (size_t i = 0; i < BB_DATA_MAX_BINDINGS; i++) {
        if (!s_slots[i].in_use) return &s_slots[i];
    }
    return NULL;
}

// Shared plain-fill thunk (B1-1415). See bb_data.h's own doc comment for the
// full rationale/extension-point note.
bb_err_t bb_data_gather_plain(void *dst, const bb_data_gather_args_t *args)
{
    if (!args || !args->ctx) return BB_ERR_INVALID_ARG;

    const bb_data_plain_fill_ctx_t *plain = (const bb_data_plain_fill_ctx_t *)args->ctx;
    if (!plain->fill) return BB_ERR_INVALID_ARG;

    return plain->fill(dst);
}

bb_err_t bb_data_bind(const bb_data_binding_t *binding)
{
    if (!binding || !binding->key) return BB_ERR_INVALID_ARG;
    // Dual-shape contract (B1-1418): a binding is EITHER a scalar producer
    // (desc && gather, the original shape) OR a rows-only table producer
    // (!desc && !gather && rows -- no scalar egress at all). Reject every
    // other combination: desc without gather or gather without desc (a
    // half-specified scalar pair is never useful), and everything-absent
    // (no desc, no gather, no rows -- a binding with nothing to render).
    bool scalar_shape    = binding->desc && binding->gather;
    bool rows_only_shape = !binding->desc && !binding->gather && binding->rows;
    if (!scalar_shape && !rows_only_shape) return BB_ERR_INVALID_ARG;
    // `apply` durably writes through slot->desc->snap_size
    // (bb_data_commit()) -- meaningless, and a NULL-deref waiting to happen,
    // on a rows-only binding that has no desc at all. bb_data_parse() only
    // gates on slot->apply being set (never slot->desc), so an apply-set,
    // desc-less binding would sail through parse and NULL-deref in commit;
    // this rejects that shape at bind time instead.
    if (!binding->desc && binding->apply) return BB_ERR_INVALID_ARG;
    if (strlen(binding->key) >= BB_DATA_KEY_MAX) return BB_ERR_INVALID_ARG;
    if (binding->rows) {
        if (!binding->rows->row_desc || !binding->rows->row_gather) return BB_ERR_INVALID_ARG;
        if (binding->rows->row_size != binding->rows->row_desc->snap_size) return BB_ERR_INVALID_ARG;
    }
    if (binding->gather == bb_data_gather_plain) {
        if (!binding->ctx) return BB_ERR_INVALID_ARG;
        if (!((const bb_data_plain_fill_ctx_t *)binding->ctx)->fill) return BB_ERR_INVALID_ARG;
    }

    bb_data_slot_t *slot = (bb_data_slot_t *)bb_registry_lookup(&s_bb_data_registry, binding->key);
    if (!slot) {
        slot = find_free_slot();
        if (!slot) {
            // Name the rejected key -- the generic composition-root log line
            // ("bb_data_bind (component=bb_data) failed (257)") gives no way
            // to tell WHICH binding was lost; this is the only place that
            // knows.
            bb_log_e(TAG, "bind failed: table full (%d slots), key \"%s\" dropped",
                     (int)BB_DATA_MAX_BINDINGS, binding->key);
            return BB_ERR_NO_SPACE;
        }

        strncpy(slot->key, binding->key, sizeof(slot->key) - 1);
        slot->key[sizeof(slot->key) - 1] = '\0';

        bb_err_t rc = bb_registry_register(&s_bb_data_registry, slot->key, slot);
        if (rc != BB_OK) return rc;  // LCOV_EXCL_BR_LINE -- find_free_slot()'s in_use scan and the registry's own capacity move in lockstep (every register() here claims exactly one slot), ruling out NO_SPACE/duplicate-key; the lazy lock-init behind bb_registry_register() can still fail transiently (lock-unavailable), a real but not host-reproducible branch (see bb_registry.c) -- the checked return above is what propagates it.
        slot->in_use = true;
    }

    slot->desc        = binding->desc;
    slot->gather      = binding->gather;
    slot->apply       = binding->apply;
    slot->ctx         = binding->ctx;
    slot->replay_kind = binding->replay_kind;
    slot->rows        = binding->rows;

    return BB_OK;
}

bb_err_t bb_data_render(const bb_data_render_req_t *req)
{
    if (!req || !req->key || !req->scratch || !req->buf || !req->out_len) return BB_ERR_INVALID_ARG;

    bb_data_slot_t *slot = (bb_data_slot_t *)bb_registry_lookup(&s_bb_data_registry, req->key);
    if (!slot) return BB_ERR_NOT_FOUND;

    bb_serialize_render_fn render = bb_serialize_format_get_render(req->fmt);
    if (!render) return BB_ERR_UNSUPPORTED;

    // NOT dead code (B1-1418): a rows-only binding (desc == NULL, gather ==
    // NULL -- see bb_data_bind()'s dual-shape contract) has no scalar egress
    // at all, so this generic scalar path can't serve it -- without this
    // guard the deref below would NULL-crash. This is reachable in practice,
    // not just in theory: bb_data_http_attach*() is a separate string-keyed
    // registry that never inspects a binding's shape, and its SSE
    // broadcaster sweep (bb_data_http_common.c) calls bb_data_render()
    // generically for every attached key via espidf_render_fn() -- so a
    // composition root that attaches a rows-only key hits this on the very
    // next sweep.
    //
    // Checking `desc` alone suffices: bb_data_bind()'s dual-shape contract
    // guarantees desc and gather are always both set or both NULL on any
    // slot that made it past bind (a desc-without-gather or gather-without-
    // desc binding is rejected there) -- checking both here would add an
    // OR-branch gcov can never actually exercise as split (the mismatched
    // desc/gather combination this guard would otherwise need to catch is
    // provably unreachable).
    if (!slot->desc) return BB_ERR_UNSUPPORTED;

    if (req->scratch_cap < slot->desc->snap_size) return BB_ERR_NO_SPACE;

    bb_data_gather_args_t args = { .ctx = slot->ctx, .query = req->query };
    bb_err_t rc = slot->gather(req->scratch, &args);
    if (rc != BB_OK) return rc;

    return render(slot->desc, req->scratch, req->buf, req->buf_cap, req->out_len);
}

// Bounded, on-stack per-row line buffer -- an internal rendering scratch,
// not a caller-tunable knob (no Kconfig bridge, per B1-1414's design:
// row_scratch/max_rows sizing stays entirely caller-owned). 160 is an
// independent, bb_data-owned bound -- it deliberately does NOT bridge
// bb_serialize_console's CONFIG_BB_SERIALIZE_CONSOLE_LINE_MAX_BYTES (it
// only happens to match that component's own default today): bb_data must
// stay format-neutral, and bb_data_render_rows() dispatches through the
// generic bb_serialize_render_fn seam, so taking a compile-time dependency
// on one specific format component's Kconfig would be a layering
// violation. A rendered row that exceeds this bound is silently truncated
// (still NUL-terminated) by the underlying render fn -- see
// bb_serialize_console_render()'s own "truncated-but-NUL-terminated is a
// success, not an error" contract -- `emit_row` still fires for that row,
// with `len` reflecting the truncated length, not the full content.
#define BB_DATA_ROW_LINE_MAX_BYTES 160

bb_err_t bb_data_render_rows(const bb_data_render_rows_req_t *req)
{
    if (!req || !req->key || !req->row_scratch || !req->emit_row) return BB_ERR_INVALID_ARG;

    bb_data_slot_t *slot = (bb_data_slot_t *)bb_registry_lookup(&s_bb_data_registry, req->key);
    if (!slot) return BB_ERR_NOT_FOUND;

    // Table rendering is console-only -- N JSON fragments would not be a
    // valid array (see bb_data_render_rows_req_t's own doc comment).
    if (req->fmt != BB_FORMAT_CONSOLE) return BB_ERR_UNSUPPORTED;

    if (!slot->rows) return BB_ERR_UNSUPPORTED;

    bb_serialize_render_fn render = bb_serialize_format_get_render(BB_FORMAT_CONSOLE);
    if (!render) return BB_ERR_UNSUPPORTED;

    bb_data_gather_args_t args = { .ctx = slot->ctx, .query = req->query };
    size_t n = slot->rows->row_gather(req->row_scratch, req->max_rows, &args);
    // Clamps the reported row count for THIS render loop only -- it does NOT
    // protect req->row_scratch from a row_gather that already wrote past
    // max_rows before returning (see bb_data_row_gather_fn's own MUST-NOT
    // doc comment in bb_data.h: that buffer-safety obligation is entirely
    // the hook's, not something this clamp can retroactively provide).
    if (n > req->max_rows) n = req->max_rows;

    const uint8_t *rows = (const uint8_t *)req->row_scratch;
    for (size_t i = 0; i < n; i++) {
        char   line[BB_DATA_ROW_LINE_MAX_BYTES];
        size_t out_len = 0;
        bb_err_t rc = render(slot->rows->row_desc, rows + (i * slot->rows->row_size),
                              line, sizeof(line), &out_len);
        if (rc != BB_OK) return rc;

        req->emit_row(line, out_len, i, req->emit_ctx);
    }

    return BB_OK;
}

bb_err_t bb_data_parse(const bb_data_parse_req_t *req, bb_data_parsed_t *out_parsed)
{
    if (!req || !req->key || !req->parse_scratch || !out_parsed) return BB_ERR_INVALID_ARG;
    if (req->body_len > 0 && !req->body) return BB_ERR_INVALID_ARG;

    bb_data_slot_t *slot = (bb_data_slot_t *)bb_registry_lookup(&s_bb_data_registry, req->key);
    if (!slot) return BB_ERR_NOT_FOUND;

    bb_serialize_parse_fn parse = bb_serialize_format_get_parse(req->fmt);
    if (!parse) return BB_ERR_UNSUPPORTED;

    if (!slot->apply) return BB_ERR_UNSUPPORTED;

    bb_serialize_populate_t src;
    bb_err_t rc = parse(req->body, req->body_len, req->parse_scratch, req->parse_scratch_cap, &src);
    if (rc != BB_OK) return rc;

    out_parsed->_binding = slot;
    out_parsed->_src      = src;
    return BB_OK;
}

bb_err_t bb_data_commit(const bb_data_parsed_t *parsed, const bb_data_commit_req_t *req)
{
    if (!parsed || !parsed->_binding || !req || !req->dst_scratch) return BB_ERR_INVALID_ARG;

    bb_data_slot_t *slot = (bb_data_slot_t *)parsed->_binding;

    // NOT dead code (B1-1418 residual): bb_data_parse() gates only on
    // slot->apply, never slot->desc, and bb_data_bind() allows rebinding an
    // already-bound key to the desc-less rows-only shape (which also forces
    // apply back to NULL -- see bb_data_bind()'s dual-shape contract). A
    // caller that parses, then rebinds the SAME key rows-only before
    // committing, hands this function a `parsed` whose captured slot now has
    // desc == NULL -- without this guard, the deref below and the
    // slot->gather() call in the BB_DATA_APPLY_PATCH branch would both
    // NULL-crash. Checking `desc` alone suffices for `gather` too: every bind
    // (initial or rebind) sets desc/gather together from the same binding
    // (scalar shape requires both non-NULL, rows-only requires both NULL --
    // see bb_data_bind()), so they can never diverge on any given slot. This
    // mirrors bb_data_render()'s own `!slot->desc` guard above.
    if (!slot->desc) return BB_ERR_UNSUPPORTED;

    if (req->dst_scratch_cap < slot->desc->snap_size) return BB_ERR_NO_SPACE;

    if (req->mode == BB_DATA_APPLY_PATCH) {
        bb_data_gather_args_t gargs = { .ctx = slot->ctx, .query = NULL };
        bb_err_t rc = slot->gather(req->dst_scratch, &gargs);
        if (rc != BB_OK) return rc;
    } else {
        memset(req->dst_scratch, 0, slot->desc->snap_size);
    }

    bb_err_t rc = bb_serialize_populate(slot->desc, req->dst_scratch, &parsed->_src);
    if (rc != BB_OK) return rc;

    bb_data_apply_args_t aargs = { .ctx = slot->ctx };
    return slot->apply(req->dst_scratch, &aargs);
}

bb_err_t bb_data_apply(const bb_data_apply_req_t *req)
{
    // dst_scratch is validated here (rather than left solely to
    // bb_data_commit()) so a NULL dst_scratch is reported up front as
    // BB_ERR_INVALID_ARG without spending a parse cycle first -- matches
    // the flat-call contract's own documented NULL-arg surface.
    if (!req || !req->dst_scratch) return BB_ERR_INVALID_ARG;

    bb_data_parse_req_t parse_req = {
        .fmt               = req->fmt,
        .key               = req->key,
        .body              = req->body,
        .body_len          = req->body_len,
        .parse_scratch     = req->parse_scratch,
        .parse_scratch_cap = req->parse_scratch_cap,
    };
    bb_data_parsed_t parsed;
    bb_err_t rc = bb_data_parse(&parse_req, &parsed);
    if (rc != BB_OK) return rc;

    bb_data_commit_req_t commit_req = {
        .mode            = req->mode,
        .dst_scratch     = req->dst_scratch,
        .dst_scratch_cap = req->dst_scratch_cap,
    };
    return bb_data_commit(&parsed, &commit_req);
}

bb_err_t bb_data_binding_replay_kind(const char *key, bb_data_replay_kind_t *out_kind)
{
    if (!key || !out_kind) return BB_ERR_INVALID_ARG;

    bb_data_slot_t *slot = (bb_data_slot_t *)bb_registry_lookup(&s_bb_data_registry, key);
    if (!slot) return BB_ERR_NOT_FOUND;

    *out_kind = slot->replay_kind;
    return BB_OK;
}

bb_err_t bb_data_touch(const char *key)
{
    if (!key) return BB_ERR_INVALID_ARG;

    bb_data_slot_t *slot = (bb_data_slot_t *)bb_registry_lookup(&s_bb_data_registry, key);
    if (!slot) return BB_ERR_NOT_FOUND;

    atomic_fetch_add_explicit(&slot->generation, 1, memory_order_release);
    return BB_OK;
}

bb_err_t bb_data_generation(const char *key, uint32_t *out_gen)
{
    if (!key || !out_gen) return BB_ERR_INVALID_ARG;

    bb_data_slot_t *slot = (bb_data_slot_t *)bb_registry_lookup(&s_bb_data_registry, key);
    if (!slot) return BB_ERR_NOT_FOUND;

    *out_gen = atomic_load_explicit(&slot->generation, memory_order_acquire);
    return BB_OK;
}

#ifdef BB_DATA_TESTING
void bb_data_test_reset(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    bb_registry_reset(&s_bb_data_registry);
}
#endif

// ---------------------------------------------------------------------------
// Scratch acquire/release (B1-1287) -- see bb_data.h's own doc comments for
// the contract/SAFETY argument. One shared, file-scope static pair; a single
// `s_scratch_in_use` flag is the ENTIRE reentrancy guard (single enforcement
// point, per the PR's own design -- no per-call-site guard duplication).
// ---------------------------------------------------------------------------

static char s_scratch_body[BB_DATA_SCRATCH_BODY_BYTES];
static char s_scratch_parse[BB_DATA_SCRATCH_PARSE_BYTES];
static bool s_scratch_in_use = false;

bb_err_t bb_data_scratch_acquire(bb_data_scratch_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;

    if (s_scratch_in_use) {
#ifndef BB_DATA_TESTING
        // Loud trap in a real (non-test) build -- see bb_data_scratch_
        // acquire()'s own doc comment in bb_data.h for why this is compiled
        // out under BB_DATA_TESTING: a process-aborting assert() is not
        // something a Unity host test can observe as a passing result, so a
        // host test instead exercises only the fail-closed
        // BB_ERR_INVALID_STATE return below.
        assert(false && "bb_data_scratch_acquire: reentrant acquire -- see SAFETY note in bb_data.h");
#endif
        return BB_ERR_INVALID_STATE;
    }

    s_scratch_in_use = true;
    out->body              = s_scratch_body;
    out->body_cap           = sizeof(s_scratch_body);
    out->parse_scratch      = s_scratch_parse;
    out->parse_scratch_cap  = sizeof(s_scratch_parse);
    return BB_OK;
}

void bb_data_scratch_release(void)
{
    s_scratch_in_use = false;
}

#ifdef BB_DATA_TESTING
void bb_data_scratch_test_reset(void)
{
    s_scratch_in_use = false;
}
#endif

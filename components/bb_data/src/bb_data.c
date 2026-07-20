// bb_data core binding table (B1-832). See bb_data.h for the seam contract.
// Stores the key -> (desc, gather) binding table in a bb_registry instance
// (name-keyed, one bb_data slot's address per entry) -- no hand-rolled
// fixed array + linear string scan.
#include "bb_data.h"

#include "bb_registry.h"
#include "bb_serialize_format.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

// One key's stored binding -- a BY-VALUE copy of the caller's
// bb_data_binding_t (the caller's own struct may be a stack temporary), plus
// an owned copy of the key string (a stable, static-storage name pointer for
// the bb_registry entry -- the registry only borrows `name`, it never copies
// it).
typedef struct {
    bool                       in_use;
    char                       key[BB_DATA_KEY_MAX];
    const bb_serialize_desc_t *desc;
    bb_data_gather_fn          gather;
    bb_data_apply_fn           apply;
    void                      *ctx;
    bb_data_replay_kind_t      replay_kind;
    _Atomic uint32_t           generation;
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

bb_err_t bb_data_bind(const bb_data_binding_t *binding)
{
    if (!binding || !binding->key || !binding->desc || !binding->gather) return BB_ERR_INVALID_ARG;
    if (strlen(binding->key) >= BB_DATA_KEY_MAX) return BB_ERR_INVALID_ARG;

    bb_data_slot_t *slot = (bb_data_slot_t *)bb_registry_lookup(&s_bb_data_registry, binding->key);
    if (!slot) {
        slot = find_free_slot();
        if (!slot) return BB_ERR_NO_SPACE;

        strncpy(slot->key, binding->key, sizeof(slot->key) - 1);
        slot->key[sizeof(slot->key) - 1] = '\0';

        bb_err_t rc = bb_registry_register(&s_bb_data_registry, slot->key, slot);
        if (rc != BB_OK) return rc;  // LCOV_EXCL_BR_LINE -- find_free_slot()'s in_use scan and the registry's own capacity move in lockstep (every register() here claims exactly one slot); this can never actually diverge.
        slot->in_use = true;
    }

    slot->desc        = binding->desc;
    slot->gather      = binding->gather;
    slot->apply       = binding->apply;
    slot->ctx         = binding->ctx;
    slot->replay_kind = binding->replay_kind;

    return BB_OK;
}

bb_err_t bb_data_render(const bb_data_render_req_t *req)
{
    if (!req || !req->key || !req->scratch || !req->buf || !req->out_len) return BB_ERR_INVALID_ARG;

    bb_data_slot_t *slot = (bb_data_slot_t *)bb_registry_lookup(&s_bb_data_registry, req->key);
    if (!slot) return BB_ERR_NOT_FOUND;

    bb_serialize_render_fn render = bb_serialize_format_get_render(req->fmt);
    if (!render) return BB_ERR_UNSUPPORTED;

    if (req->scratch_cap < slot->desc->snap_size) return BB_ERR_NO_SPACE;

    bb_data_gather_args_t args = { .ctx = slot->ctx, .query = req->query };
    bb_err_t rc = slot->gather(req->scratch, &args);
    if (rc != BB_OK) return rc;

    return render(slot->desc, req->scratch, req->buf, req->buf_cap, req->out_len);
}

bb_err_t bb_data_apply(const bb_data_apply_req_t *req)
{
    if (!req || !req->key || !req->parse_scratch || !req->dst_scratch) return BB_ERR_INVALID_ARG;
    if (req->body_len > 0 && !req->body) return BB_ERR_INVALID_ARG;

    bb_data_slot_t *slot = (bb_data_slot_t *)bb_registry_lookup(&s_bb_data_registry, req->key);
    if (!slot) return BB_ERR_NOT_FOUND;

    bb_serialize_parse_fn parse = bb_serialize_format_get_parse(req->fmt);
    if (!parse) return BB_ERR_UNSUPPORTED;

    if (!slot->apply) return BB_ERR_UNSUPPORTED;

    if (req->dst_scratch_cap < slot->desc->snap_size) return BB_ERR_NO_SPACE;

    if (req->mode == BB_DATA_APPLY_PATCH) {
        bb_data_gather_args_t gargs = { .ctx = slot->ctx, .query = NULL };
        bb_err_t rc = slot->gather(req->dst_scratch, &gargs);
        if (rc != BB_OK) return rc;
    } else {
        memset(req->dst_scratch, 0, slot->desc->snap_size);
    }

    bb_serialize_populate_t src;
    bb_err_t rc = parse(req->body, req->body_len, req->parse_scratch, req->parse_scratch_cap, &src);
    if (rc != BB_OK) return rc;

    rc = bb_serialize_populate(slot->desc, req->dst_scratch, &src);
    if (rc != BB_OK) return rc;

    bb_data_apply_args_t aargs = { .ctx = slot->ctx };
    return slot->apply(req->dst_scratch, &aargs);
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

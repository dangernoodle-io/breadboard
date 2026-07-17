// bb_data core binding table (B1-832). See bb_data.h for the seam contract.
// Stores the key -> (desc, gather) binding table in a bb_registry instance
// (name-keyed, one bb_data slot's address per entry) -- no hand-rolled
// fixed array + linear string scan.
#include "bb_data.h"

#include "bb_registry.h"
#include "bb_serialize_format.h"

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
    void                      *ctx;
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

    slot->desc   = binding->desc;
    slot->gather = binding->gather;
    slot->ctx    = binding->ctx;

    return BB_OK;
}

bb_err_t bb_data_render(bb_format_t fmt, const char *key,
                        void *scratch, size_t scratch_cap,
                        char *buf, size_t cap, size_t *out_len)
{
    if (!key || !scratch || !buf || !out_len) return BB_ERR_INVALID_ARG;

    bb_data_slot_t *slot = (bb_data_slot_t *)bb_registry_lookup(&s_bb_data_registry, key);
    if (!slot) return BB_ERR_NOT_FOUND;

    bb_serialize_render_fn render = bb_serialize_format_get_render(fmt);
    if (!render) return BB_ERR_UNSUPPORTED;

    if (scratch_cap < slot->desc->snap_size) return BB_ERR_NO_SPACE;

    bb_err_t rc = slot->gather(scratch, slot->ctx);
    if (rc != BB_OK) return rc;

    return render(slot->desc, scratch, buf, cap, out_len);
}

#ifdef BB_DATA_TESTING
void bb_data_test_reset(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    bb_registry_reset(&s_bb_data_registry);
}
#endif

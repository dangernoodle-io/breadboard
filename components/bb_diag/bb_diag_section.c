// Portable section registry + pure dispatch helpers for the bb_diag section
// registry (B1-diag-dissolution PR3). See bb_diag_section.h for the public
// seam contract and bb_diag_section_priv.h for the internals shared with
// the ESP-IDF dispatcher and host tests. Stores the name -> section table in
// a bb_registry instance (name-keyed), same shape as bb_data.c's own
// binding table -- no hand-rolled fixed array + linear string scan.
#include "bb_diag_section_priv.h"

#include "bb_log.h"
#include "bb_registry.h"

#include <string.h>

static const char *TAG = "bb_diag_section";

#define BB_DIAG_SECTION_URI_PREFIX     "/api/diag/"
#define BB_DIAG_SECTION_URI_PREFIX_LEN (sizeof(BB_DIAG_SECTION_URI_PREFIX) - 1)

// One name's stored section -- a BY-VALUE copy of the caller's
// bb_diag_section_t (the caller's own struct may be a stack temporary),
// plus an owned copy of the name string (a stable, static-storage name
// pointer for the bb_registry entry -- the registry only borrows `name`,
// it never copies it).
typedef struct {
    bool               in_use;
    char               name[BB_DIAG_SECTION_NAME_MAX];
    bb_diag_section_t  section;
} bb_diag_section_slot_t;

static bb_diag_section_slot_t s_slots[BB_DIAG_SECTION_TABLE_CAP];

BB_REGISTRY_DEFINE_TAGGED(s_bb_diag_section_registry, BB_DIAG_SECTION_TABLE_CAP, "bb_diag_section");

// First slot with in_use == false. Guaranteed to find one whenever
// bb_registry_count(&s_bb_diag_section_registry) < BB_DIAG_SECTION_TABLE_CAP,
// since every successful bb_registry_register() call below is paired 1:1
// with claiming exactly one slot here (and neither table supports removal).
static bb_diag_section_slot_t *find_free_slot(void)
{
    for (size_t i = 0; i < BB_DIAG_SECTION_TABLE_CAP; i++) {
        if (!s_slots[i].in_use) return &s_slots[i];
    }
    return NULL;
}

bb_err_t bb_diag_register_section(const bb_diag_section_t *section)
{
    if (!section || !section->name || !section->snap_desc || !section->fill) return BB_ERR_INVALID_ARG;
    if (strlen(section->name) >= BB_DIAG_SECTION_NAME_MAX) return BB_ERR_INVALID_ARG;
    if (section->n_query_keys > BB_SERIALIZE_QUERY_MAX_PARAMS) return BB_ERR_INVALID_ARG;
    if (section->n_query_keys > 0 && !section->query_keys) return BB_ERR_INVALID_ARG;

    if (section->snap_desc->snap_size > BB_DIAG_SECTION_SCRATCH_BYTES) {
        bb_log_e(TAG, "register('%s'): snap_size=%u exceeds scratch=%u -- "
                 "raise CONFIG_BB_DIAG_SECTION_SCRATCH_BYTES or shrink the snapshot",
                 section->name, (unsigned)section->snap_desc->snap_size,
                 (unsigned)BB_DIAG_SECTION_SCRATCH_BYTES);
        return BB_ERR_NO_SPACE;
    }

    // Reject-on-duplicate, first-wins -- checked explicitly (rather than
    // relying solely on bb_registry_register()'s own duplicate rejection)
    // so a duplicate name is reported as BB_ERR_INVALID_STATE even when the
    // table also happens to be full, matching the precedence documented in
    // bb_diag_section.h.
    if (bb_registry_lookup(&s_bb_diag_section_registry, section->name)) {
        return BB_ERR_INVALID_STATE;
    }

    bb_diag_section_slot_t *slot = find_free_slot();
    if (!slot) return BB_ERR_NO_SPACE;

    strncpy(slot->name, section->name, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = '\0';

    bb_err_t rc = bb_registry_register(&s_bb_diag_section_registry, slot->name, slot);
    if (rc != BB_OK) return rc;  // LCOV_EXCL_BR_LINE -- the lookup+find_free_slot pair above already proved this name is absent and a slot is free; bb_registry_register() cannot fail here.

    slot->in_use = true;
    slot->section = *section;
    return BB_OK;
}

const bb_diag_section_t *bb_diag_section_find(const char *name)
{
    if (!name) return NULL;

    bb_diag_section_slot_t *slot =
        (bb_diag_section_slot_t *)bb_registry_lookup(&s_bb_diag_section_registry, name);
    return slot ? &slot->section : NULL;
}

bb_err_t bb_diag_section_name_from_uri(const char *uri, char *out, size_t out_cap)
{
    if (!uri || !out || out_cap == 0) return BB_ERR_INVALID_ARG;

    if (strncmp(uri, BB_DIAG_SECTION_URI_PREFIX, BB_DIAG_SECTION_URI_PREFIX_LEN) != 0) {
        return BB_ERR_NOT_FOUND;
    }

    strncpy(out, uri + BB_DIAG_SECTION_URI_PREFIX_LEN, out_cap - 1);
    out[out_cap - 1] = '\0';
    return BB_OK;
}

bb_err_t bb_diag_section_build_query(const bb_diag_section_t *sec,
                                      bb_diag_query_getter_fn get, void *get_ctx,
                                      char *value_scratch, bb_serialize_query_t *out)
{
    if (!sec || !get || !value_scratch || !out) return BB_ERR_INVALID_ARG;

    out->count = 0;
    for (size_t i = 0; i < sec->n_query_keys; i++) {
        const char *key = sec->query_keys[i];
        char       *value = value_scratch + (i * BB_DIAG_SECTION_QUERY_VALUE_BYTES);

        if (!get(get_ctx, key, value, BB_DIAG_SECTION_QUERY_VALUE_BYTES)) continue;

        out->params[out->count].key   = key;
        out->params[out->count].value = value;
        out->count++;
    }
    return BB_OK;
}

#ifdef BB_DIAG_SECTION_TESTING
void bb_diag_section_test_reset(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    bb_registry_reset(&s_bb_diag_section_registry);
}
#endif

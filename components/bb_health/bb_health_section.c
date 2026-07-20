// Portable composer-shaped section registry for the future /api/health
// document (B1-1096 PR-1 of 6, epic B1-1054). See bb_health_section.h for
// the public seam contract and bb_health_section_priv.h for the
// Kconfig-bridged scratch budget. Stores the name -> section table in a
// bb_registry instance (name-keyed), same shape as bb_diag_section.c's own
// table -- no hand-rolled fixed array + linear string scan.
#include "bb_health_section_priv.h"

#include "bb_log.h"
#include "bb_registry.h"

#include <string.h>

static const char *TAG = "bb_health_section";

// One name's stored section -- a BY-VALUE copy of the caller's
// bb_health_section_t (the caller's own struct may be a stack temporary),
// plus an owned copy of the name string (a stable, static-storage name
// pointer for the bb_registry entry -- the registry only borrows `name`,
// it never copies it).
typedef struct {
    bool                in_use;
    char                name[BB_HEALTH_SECTION_NAME_MAX];
    bb_health_section_t section;
} bb_health_section_slot_t;

static bb_health_section_slot_t s_slots[BB_HEALTH_SECTION_TABLE_CAP];

BB_REGISTRY_DEFINE_TAGGED(s_bb_health_section_registry, BB_HEALTH_SECTION_TABLE_CAP, "bb_health_section");

// First slot with in_use == false. Guaranteed to find one whenever
// bb_registry_count(&s_bb_health_section_registry) < BB_HEALTH_SECTION_TABLE_CAP,
// since every successful bb_registry_register() call below is paired 1:1
// with claiming exactly one slot here (and neither table supports removal).
static bb_health_section_slot_t *find_free_slot(void)
{
    for (size_t i = 0; i < BB_HEALTH_SECTION_TABLE_CAP; i++) {
        if (!s_slots[i].in_use) return &s_slots[i];
    }
    return NULL;
}

bb_err_t bb_health_section_register(const bb_health_section_t *section)
{
    if (!section || !section->name || !section->snap_desc || !section->fill) return BB_ERR_INVALID_ARG;
    if (strlen(section->name) >= BB_HEALTH_SECTION_NAME_MAX) return BB_ERR_INVALID_ARG;

    if (section->snap_desc->snap_size > BB_HEALTH_SECTION_SCRATCH_BYTES) {
        bb_log_e(TAG, "register('%s'): snap_size=%u exceeds scratch=%u -- "
                 "raise CONFIG_BB_HEALTH_SECTION_SCRATCH_BYTES or shrink the snapshot",
                 section->name, (unsigned)section->snap_desc->snap_size,
                 (unsigned)BB_HEALTH_SECTION_SCRATCH_BYTES);
        return BB_ERR_NO_SPACE;
    }

    // Reject-on-duplicate, first-wins -- checked explicitly (rather than
    // relying solely on bb_registry_register()'s own duplicate rejection)
    // so a duplicate name is reported as BB_ERR_INVALID_STATE even when the
    // table also happens to be full, matching bb_diag_register_section()'s
    // precedent and precedence order.
    if (bb_registry_lookup(&s_bb_health_section_registry, section->name)) {
        return BB_ERR_INVALID_STATE;
    }

    bb_health_section_slot_t *slot = find_free_slot();
    if (!slot) return BB_ERR_NO_SPACE;

    strncpy(slot->name, section->name, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = '\0';

    // bb_registry_register() itself reports BB_ERR_INVALID_STATE when the
    // registry is frozen -- this is where a freeze-after-server-start
    // rejection surfaces (bb_health_section_freeze() below).
    bb_err_t rc = bb_registry_register(&s_bb_health_section_registry, slot->name, slot);
    if (rc != BB_OK) return rc;

    slot->in_use = true;
    slot->section = *section;
    return BB_OK;
}

void bb_health_section_freeze(void)
{
    bb_registry_freeze(&s_bb_health_section_registry);
}

#ifdef BB_HEALTH_SECTION_TESTING
void bb_health_section_test_reset(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    bb_registry_reset(&s_bb_health_section_registry);
}

const bb_health_section_t *bb_health_section_test_find(const char *name)
{
    if (!name) return NULL;

    bb_health_section_slot_t *slot =
        (bb_health_section_slot_t *)bb_registry_lookup(&s_bb_health_section_registry, name);
    return slot ? &slot->section : NULL;
}
#endif

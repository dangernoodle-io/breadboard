#include "bb_info.h"
#include "bb_section.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../../components/bb_info/bb_info_schema_priv.h"
#include "../../components/bb_info/src/bb_info_build_priv.h"

// File-scope section registry for /api/info.
static bb_section_registry_t s_info_reg = { .tag = "bb_info" };

// Capability registry (host twin).
static const char *s_capabilities[BB_INFO_MAX_CAPABILITIES];
static int         s_capability_count = 0;
static bool        s_cap_frozen       = false;

// Cached assembled schema (lazy, freed on reset).
static char *s_assembled_schema = NULL;

#ifdef BB_INFO_TESTING
#include "bb_info_test.h"
#endif

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_info_register_section(const char *name,
                                   bb_section_get_fn get,
                                   void *ctx,
                                   const char *schema_props)
{
    return bb_section_register(&s_info_reg, name, get, NULL, ctx, schema_props);
}

void bb_info_register_capability(const char *name)
{
    if (!name || !name[0]) return;
    if (s_cap_frozen) return;
    for (int i = 0; i < s_capability_count; i++) {
        if (strcmp(s_capabilities[i], name) == 0) return;
    }
    if (s_capability_count >= BB_INFO_MAX_CAPABILITIES) return;
    s_capabilities[s_capability_count++] = name;
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_INFO_TESTING

void bb_info_freeze_for_test(void)
{
    s_cap_frozen = true;
    bb_section_freeze(&s_info_reg);
}

void bb_info_invoke_sections_for_test(bb_json_t root)
{
    bb_section_build_get(&s_info_reg, root);
}

void bb_info_reset_for_test(void)
{
    memset(&s_info_reg, 0, sizeof(s_info_reg));
    s_info_reg.tag = "bb_info";
    memset(s_capabilities, 0, sizeof(s_capabilities));
    s_capability_count = 0;
    s_cap_frozen       = false;
    free(s_assembled_schema);
    s_assembled_schema = NULL;
}

const char *bb_info_get_assembled_schema(void)
{
    if (!s_assembled_schema) {
        s_assembled_schema = bb_section_freeze_and_assemble(&s_info_reg, k_info_schema_base, k_info_schema_suffix);
    }
    return s_assembled_schema;
}

#endif /* BB_INFO_TESTING */

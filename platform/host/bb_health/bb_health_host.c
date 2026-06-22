#include "bb_health.h"
#include "bb_section.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../../../components/bb_health/bb_health_schema_priv.h"

// File-scope section registry for /api/health.
static bb_section_registry_t s_health_reg = { .tag = "bb_health" };

// Cached assembled schema (lazy, freed on reset).
static char *s_assembled_schema = NULL;

#ifdef BB_HEALTH_TESTING
#include "bb_health_test.h"
#endif

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_health_register_section(const char *name,
                                     bb_section_get_fn get,
                                     void *ctx,
                                     const char *schema_props)
{
    return bb_section_register(&s_health_reg, name, get, NULL, ctx, schema_props);
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_HEALTH_TESTING

void bb_health_freeze_for_test(void)
{
    bb_section_freeze(&s_health_reg);
}

void bb_health_invoke_sections_for_test(bb_json_t root)
{
    bb_section_build_get(&s_health_reg, root);
}

void bb_health_reset_for_test(void)
{
    memset(&s_health_reg, 0, sizeof(s_health_reg));
    s_health_reg.tag = "bb_health";
    free(s_assembled_schema);
    s_assembled_schema = NULL;
}

const char *bb_health_get_assembled_schema(void)
{
    if (!s_assembled_schema) {
        s_assembled_schema = bb_section_freeze_and_assemble(&s_health_reg, k_health_base, k_health_suffix);
    }
    return s_assembled_schema;
}

#endif /* BB_HEALTH_TESTING */

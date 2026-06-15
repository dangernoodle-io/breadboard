// bb_sensors host twin — test hooks for /api/sensors.
// Provides section registry, schema assembly, and test-isolation reset.
#include "bb_sensors.h"
#include "bb_section.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../../../components/bb_sensors/bb_sensors_schema_priv.h"

// File-scope section registry for /api/sensors.
static bb_section_registry_t s_sensors_reg = { .tag = "bb_sensors" };

// Cached assembled schema (lazy, freed on reset).
static char *s_assembled_schema = NULL;

#ifdef BB_SENSORS_TESTING
#include "bb_sensors.h"
#endif

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_sensors_register_section(const char *name,
                                      bb_section_get_fn get,
                                      bb_section_patch_fn patch,
                                      void *ctx,
                                      const char *schema_props)
{
    return bb_section_register(&s_sensors_reg, name, get, patch, ctx, schema_props);
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_SENSORS_TESTING

void bb_sensors_freeze_for_test(void)
{
    bb_section_freeze(&s_sensors_reg);
}

void bb_sensors_invoke_sections_for_test(bb_json_t root)
{
    bb_section_build_get(&s_sensors_reg, root);
}

bb_err_t bb_sensors_dispatch_patch_for_test(bb_json_t body)
{
    return bb_section_dispatch_patch(&s_sensors_reg, body);
}

void bb_sensors_reset_for_test(void)
{
    memset(&s_sensors_reg, 0, sizeof(s_sensors_reg));
    s_sensors_reg.tag = "bb_sensors";
    free(s_assembled_schema);
    s_assembled_schema = NULL;
}

const char *bb_sensors_get_assembled_schema(void)
{
    if (!s_assembled_schema) {
        s_assembled_schema = bb_section_assemble_schema(
            &s_sensors_reg, k_sensors_base, k_sensors_suffix);
    }
    return s_assembled_schema;
}

#endif /* BB_SENSORS_TESTING */

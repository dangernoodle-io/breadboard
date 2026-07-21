#include "bb_health.h"

#include <stdlib.h>

#include "../../../components/bb_health/bb_health_schema_priv.h"
#include "../../../components/bb_health/bb_health_section_priv.h"

// Cached assembled schema (lazy, freed on reset).
static char *s_assembled_schema = NULL;

#ifdef BB_HEALTH_TESTING
#include "bb_health_test.h"
#endif

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_HEALTH_TESTING

void bb_health_freeze_for_test(void)
{
    bb_health_section_freeze();
}

void bb_health_reset_for_test(void)
{
    bb_health_section_test_reset();
    free(s_assembled_schema);
    s_assembled_schema = NULL;
}

const char *bb_health_get_assembled_schema(void)
{
    if (!s_assembled_schema) {
        s_assembled_schema = bb_health_assemble_schema();
    }
    return s_assembled_schema;
}

#endif /* BB_HEALTH_TESTING */

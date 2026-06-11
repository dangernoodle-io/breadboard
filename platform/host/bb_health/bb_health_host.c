#include "bb_health.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "bb_http_extender.h"
#include "../../../components/bb_health/bb_health_schema_priv.h"

#ifdef BB_HEALTH_TESTING
#include "bb_health_test.h"
#include "bb_http_extender_test.h"
#endif

// ---------------------------------------------------------------------------
// Public bb_health extender wrappers
// ---------------------------------------------------------------------------

bb_err_t bb_health_register_extender_ex(bb_health_extender_fn fn,
                                         const char *schema_props_fragment)
{
    return bb_http_register_route_extender("health",
                                           (bb_http_extender_fn)fn,
                                           schema_props_fragment);
}

bb_err_t bb_health_register_extender(bb_health_extender_fn fn)
{
    return bb_health_register_extender_ex(fn, NULL);
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_HEALTH_TESTING

void bb_health_freeze_for_test(void)
{
    bb_http_extender_freeze();
}

void bb_health_invoke_extenders_for_test(void *root)
{
    bb_http_route_run_extenders("health", root);
}

const char *bb_health_get_assembled_schema(void)
{
    const char *cached = bb_http_extender_get_assembled_schema("health");
    if (cached) return cached;
    return bb_http_route_assemble_schema("health", k_health_base, k_health_suffix);
}

void bb_health_reset_for_test(void)
{
    // Reset is handled by bb_info_reset_for_test which calls
    // bb_http_extender_reset_for_test() — that resets ALL route extenders
    // including "health". No separate per-component reset needed here.
    // This function exists for symmetry and future use.
}

#endif /* BB_HEALTH_TESTING */

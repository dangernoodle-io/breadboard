#include "bb_info.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "bb_http_extender.h"
#include "../../components/bb_info/bb_info_schema_priv.h"

#ifdef BB_INFO_TESTING
#include "bb_info_test.h"
#include "bb_http_extender_test.h"
#endif

// Capability registry (host twin)
static const char *s_capabilities[BB_INFO_MAX_CAPABILITIES];
static int         s_capability_count = 0;
static bool        s_cap_frozen       = false;

// ---------------------------------------------------------------------------
// Public bb_info extender wrappers (back-compat thin wrappers)
// ---------------------------------------------------------------------------

bb_err_t bb_info_register_extender_ex(bb_info_extender_fn fn,
                                       const char *schema_props_fragment)
{
    return bb_http_register_route_extender("info",
                                           (bb_http_extender_fn)fn,
                                           schema_props_fragment);
}

bb_err_t bb_info_register_extender(bb_info_extender_fn fn)
{
    return bb_info_register_extender_ex(fn, NULL);
}

bb_err_t bb_health_register_extender_ex(bb_info_extender_fn fn,
                                         const char *schema_props_fragment)
{
    return bb_http_register_route_extender("health",
                                           (bb_http_extender_fn)fn,
                                           schema_props_fragment);
}

bb_err_t bb_health_register_extender(bb_info_extender_fn fn)
{
    return bb_health_register_extender_ex(fn, NULL);
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
    bb_http_extender_freeze();
}

void bb_info_invoke_extenders_for_test(void *root)
{
    bb_http_route_run_extenders("info", root);
}

void bb_health_invoke_extenders_for_test(void *root)
{
    bb_http_route_run_extenders("health", root);
}

// Health schema base/suffix (mirrors espidf bb_info.c).
static const char k_health_base[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"ok\":{\"type\":\"boolean\"},"
    "\"free_heap\":{\"type\":\"integer\"},"
    "\"validated\":{\"type\":\"boolean\"},"
    "\"network\":{\"type\":\"object\","
    "\"properties\":{"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"retry_count\":{\"type\":\"integer\"},"
    "\"mdns\":{\"type\":[\"string\",\"null\"]}}}";

static const char k_health_suffix[] =
    "},"
    "\"required\":[\"ok\",\"network\"]}";

const char *bb_health_get_assembled_extender_schema(void)
{
    // Build via generic facility. The assembled schema includes base + frags +
    // suffix; callers that previously used this to get "only the fragments"
    // now get the full health schema — which is what tests actually need for
    // the fidelity check. bb_http_route_assemble_schema caches the result.
    const char *cached = bb_http_extender_get_assembled_schema("health");
    if (cached) return cached;
    return bb_http_route_assemble_schema("health", k_health_base, k_health_suffix);
}

void bb_info_reset_for_test(void)
{
    memset(s_capabilities, 0, sizeof(s_capabilities));
    s_capability_count = 0;
    s_cap_frozen       = false;
    bb_http_extender_reset_for_test();
}

const char *bb_info_get_assembled_schema(void)
{
    const char *cached = bb_http_extender_get_assembled_schema("info");
    if (cached) return cached;
    return bb_http_route_assemble_schema("info", k_info_schema_base, k_info_schema_suffix);
}

#endif /* BB_INFO_TESTING */

// test_bb_diag_drops_wire_schema_default -- B1-1444 PR3, config-OFF
// byte-identity proof for GET /api/diag/drops' response schema.
// bb_diag_drops_wire_get_schema() is a plain production accessor (not gated
// by CONFIG_BB_OPENAPI_RUNTIME_META), so this runs in the plain [env:native]
// build (config OFF, the default) and asserts the served schema is
// BB_DIAG_DROPS_SCHEMA_LITERAL -- the SAME macro text
// platform/espidf/bb_diag_http/bb_diag_http_routes.c's config-OFF
// s_drops_get_responses[0].schema uses directly (that TU is ESP-IDF-only and
// cannot link on host).

#include "unity.h"

#include "../../components/bb_diag_http/bb_diag_drops_wire_priv.h"

static const char *const k_expected_diag_drops_schema = BB_DIAG_DROPS_SCHEMA_LITERAL;

void test_bb_diag_drops_wire_schema_default_matches_literal(void)
{
    TEST_ASSERT_EQUAL_STRING(k_expected_diag_drops_schema, bb_diag_drops_wire_get_schema());
}

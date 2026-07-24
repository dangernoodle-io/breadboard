// test_bb_diag_http_boot_wire_schema_default -- B1-1059 emit boot:
// config-OFF byte-identity proof for GET /api/diag/boot's REST envelope
// response schema. bb_diag_http_boot_wire_get_schema() is a plain
// production accessor (not gated by CONFIG_BB_OPENAPI_RUNTIME_META), so
// this runs in the plain [env:native] build (config OFF, the default) and
// asserts the served schema is BB_DIAG_BOOT_GET_SCHEMA_LITERAL -- the SAME
// macro text platform/espidf/bb_diag_http/bb_diag_http_routes.c's config-OFF
// s_boot_get_responses[0].schema uses directly (that TU is ESP-IDF-only and
// cannot link on host; this is its "live route/register schema" surrogate,
// content not pointer, KB 1492). Non-vacuous relocation proof.

#include "unity.h"

#include "../../components/bb_diag_http/bb_diag_http_boot_wire_priv.h"

static const char *const k_expected_diag_boot_get_schema = BB_DIAG_BOOT_GET_SCHEMA_LITERAL;

void test_bb_diag_http_boot_wire_schema_default_matches_literal(void)
{
    TEST_ASSERT_EQUAL_STRING(k_expected_diag_boot_get_schema, bb_diag_http_boot_wire_get_schema());
}

// test_bb_storage_http_delete_wire_schema_default -- B1-1059 emit batch C,
// site C5: config-OFF byte-identity proof for DELETE /api/diag/storage's
// 200-response schema. bb_storage_http_delete_wire_get_schema() is a plain
// production accessor (not gated by CONFIG_BB_OPENAPI_RUNTIME_META), so
// this runs in the plain [env:native] build (config OFF, the default) and
// asserts the served schema is
// BB_STORAGE_HTTP_DELETE_RESPONSE_SCHEMA_LITERAL -- the SAME macro text
// platform/espidf/bb_diag_http/bb_storage_http_routes.c's config-OFF
// s_storage_delete_responses[0].schema uses directly (that TU is
// ESP-IDF-only and cannot link on host; this is its "live route/register
// schema" surrogate, content not pointer, KB 1492). Distinct from
// test_bb_storage_http_delete_route_wiring.c's REQUEST schema (own macro,
// own accessor).

#include "unity.h"

#include "../../components/bb_diag_http/bb_storage_http_delete_wire_priv.h"

static const char *const k_expected_storage_delete_response_schema =
    BB_STORAGE_HTTP_DELETE_RESPONSE_SCHEMA_LITERAL;

void test_bb_storage_http_delete_wire_schema_default_matches_literal(void)
{
    TEST_ASSERT_EQUAL_STRING(k_expected_storage_delete_response_schema,
                              bb_storage_http_delete_wire_get_schema());
}

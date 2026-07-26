#include "unity.h"
#include "bb_display_info_wire.h"
#include "bb_serialize_meta_test.h"

#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves the "health.display" served schema's runtime-compose path
// (B1-1059 SSE PR-4) actually wires up: bb_display_info_ensure_schema_
// patched() runs exactly once and composes content byte-identical to
// bb_serialize_meta_openapi_schema()'s own proven output over
// bb_display_info_wire_desc/_meta (already golden-tested against the hand
// literal by test_bb_display_info_wire_meta_golden.c), and re-running it is
// pointer-stable (idempotent -- never re-composes once patched).
//
// UNLIKE the SSE topic schemas (diag.boot etc), this site uses the
// plain-body helper bb_serialize_meta_ensure_composed() (not bb_serialize_
// meta_ensure_topic_schema()) -- health.display's served body carries no
// top-level "title"/"x-sse-topic" prefix, see bb_display_info_wire.c's own
// banner. Consequently the expected composed body below is IDENTICAL to
// the config-OFF relocated literal (test_bb_display_info_topic_schema_
// default.c) -- B1-1179 already brought the hand literal into line with
// the meta engine's fixed object-schema shape ahead of this migration, so
// there is no config-ON tightening delta left to gain here (unlike every
// other B1-1059 compose-at-init site).
static const char *const k_expected_display_info_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"panel\":{\"type\":\"string\"},"
    "\"width\":{\"type\":\"integer\"},"
    "\"height\":{\"type\":\"integer\"},"
    "\"enabled\":{\"type\":\"boolean\"}},"
    "\"required\":[\"present\"],"
    "\"additionalProperties\":false}";

// Exercises bb_display_info_ensure_schema_patched()'s own failure contract
// directly (not via bb_display_register_info(), which is ESP_PLATFORM-
// gated and not host-testable): forces the engine (bb_serialize_meta, via
// BB_SERIALIZE_META_TESTING's fail-injection seam) to return
// BB_ERR_NO_SPACE and asserts the error propagates with the schema buffer
// left unpatched (empty), rather than composing a partial/stale schema --
// the invariant bb_display_register_info()'s CONFIG_BB_OPENAPI_RUNTIME_META
// guard relies on to skip registration on a compose failure. MUST run
// before the two success tests below: the compose-and-patch step is
// guarded/idempotent (a non-empty schema buffer short-circuits a second
// real compose), so once a prior test has successfully composed it this
// seam can no longer force a re-compose -- see test_main.c's RUN_TEST
// order.
void test_bb_display_info_topic_schema_offline_on_compose_failure(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(true);

    bb_err_t rc = bb_display_info_ensure_schema_patched();

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", bb_display_info_get_schema());

    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

void test_bb_display_info_topic_schema_matches_expected_content(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_display_info_ensure_schema_patched());

    const char *schema = bb_display_info_get_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQUAL_STRING(k_expected_display_info_schema, schema);
}

void test_bb_display_info_topic_schema_idempotent_pointer_stable(void)
{
    const char *first = bb_display_info_get_schema();
    TEST_ASSERT_EQUAL_STRING(k_expected_display_info_schema, first);

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_display_info_ensure_schema_patched();
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    const char *second = bb_display_info_get_schema();
    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_EQUAL_STRING(k_expected_display_info_schema, second);
}

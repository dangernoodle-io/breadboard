// Tests for bb_response_freeze_and_assemble.
#include "unity.h"
#include "bb_response.h"
#include "test_alloc_inject.h"

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Stub
// ---------------------------------------------------------------------------

static void stub_get(bb_json_t section, void *ctx) { (void)section; (void)ctx; }

static bb_response_registry_t make_reg(void)
{
    bb_response_registry_t r;
    memset(&r, 0, sizeof(r));
    r.tag = "test";
    return r;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_bb_response_freeze_and_assemble_non_null(void)
{
    bb_response_registry_t reg = make_reg();
    bb_response_register(&reg, "foo", stub_get, NULL, NULL, "{\"type\":\"object\"}");

    char *s = bb_response_freeze_and_assemble(
        &reg,
        "{\"type\":\"object\",\"properties\":{",
        "}}");

    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(strstr(s, "\"foo\""));
    // Registry must be frozen after the call.
    TEST_ASSERT_TRUE(reg.frozen);
    free(s);
}

void test_bb_response_freeze_and_assemble_null_on_oom(void)
{
    bb_response_registry_t reg = make_reg();
    bb_response_register(&reg, "bar", stub_get, NULL, NULL, "{\"type\":\"string\"}");

    // Inject failing malloc: fail on the first allocation (index 0).
    test_alloc_reset();
    test_alloc_fail_at = 0;
    bb_response_set_malloc(test_failing_malloc);

    char *s = bb_response_freeze_and_assemble(
        &reg,
        "{\"type\":\"object\",\"properties\":{",
        "}}");

    // Restore real malloc before assertions to avoid side-effects.
    bb_response_set_malloc(NULL);
    test_alloc_fail_at = -1;

    TEST_ASSERT_NULL(s);
    // Registry must still be frozen even on OOM.
    TEST_ASSERT_TRUE(reg.frozen);
}

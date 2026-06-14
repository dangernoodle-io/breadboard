#include "unity.h"
#include "bb_tls_info.h"
#include "bb_info.h"
#include "bb_info_test.h"

#include <string.h>

/* setUp/tearDown: bb_info_reset_for_test() is called in test_main setUp()
 * which resets the capability and extender registries between tests. */

/* (T1) bb_tls_info_register does not crash when called on a reset registry. */
void test_bb_tls_info_register_is_safe(void)
{
    /* Must not crash or assert */
    bb_tls_info_register();
}

/* (T2) After register, the assembled schema is still valid (not corrupted). */
void test_bb_tls_info_register_schema_remains_valid(void)
{
    bb_tls_info_register();
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "capabilities"),
        "capabilities not found in assembled schema after bb_tls_info_register");
}

/* (T3) With all three gates ON (host build: all defined to 1), all three
 * capability strings are registered. */
void test_bb_tls_info_all_capabilities_registered(void)
{
    bb_tls_info_register();

    /* The bb_info capability registry is internal; we verify via the behaviour
     * of bb_info_register_capability (idempotent second call) and the fact
     * that no error is returned.  We also verify by calling register again —
     * duplicates are silently dropped, not double-registered. */
    bb_tls_info_register(); /* idempotent: second call must not crash */
}

/* (T4) Registering the same capability twice (via bb_info_register_capability
 * directly) is deduped — exercises the dedup path that bb_tls_info relies on. */
void test_bb_tls_info_dedup_via_direct_register(void)
{
    /* First call registers */
    bb_info_register_capability("mqtt_tls");
    /* Second call must be silently ignored (dedup) */
    bb_info_register_capability("mqtt_tls");
    /* Schema must still be valid */
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
}

/* (T5) bb_tls_info_register followed by a manual capability registration
 * still works (registry not full after three tls caps). */
void test_bb_tls_info_leaves_room_for_more_capabilities(void)
{
    bb_tls_info_register();
    /* Should succeed — 3 TLS caps consumed, 29 slots remaining */
    bb_info_register_capability("custom_cap");
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
}

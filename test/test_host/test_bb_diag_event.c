// Host unit tests for bb_diag diag.boot serializer:
// - bb_diag_boot_serialize (bb_cache serializer, nested shape)
#include "unity.h"
#include "bb_diag_event_priv.h"
#include "bb_json.h"
#include "bb_json_test_hooks.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Helper: build a JSON string from a snap via bb_diag_boot_serialize
// ---------------------------------------------------------------------------

static char *serialize_snap(const bb_diag_boot_snap_t *snap)
{
    bb_json_t obj = bb_json_obj_new();
    if (!obj) return NULL;
    bb_diag_boot_serialize(obj, snap);
    char *str = bb_json_serialize(obj);
    bb_json_free(obj);
    return str;
}

// ---------------------------------------------------------------------------
// Normal boot (no panic)
// ---------------------------------------------------------------------------

void test_bb_diag_boot_serialize_poweron_clean(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason    = "poweron",
        .wdt_resets      = 0,
        .panic_available = false,
        .panic_boots_since = 0,
        .pending_verify  = false,
        .rolled_back     = false,
    };
    char *str = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NOT_NULL(strstr(str, "\"reset_reason\":\"poweron\""));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"wdt_resets\":0"));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"pending_verify\":false"));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"rolled_back\":false"));
    // panic object always present
    TEST_ASSERT_NOT_NULL(strstr(str, "\"panic\":"));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"available\":false"));
    // flat panic_available key must NOT appear
    TEST_ASSERT_NULL(strstr(str, "\"panic_available\""));
    bb_json_free_str(str);
}

// ---------------------------------------------------------------------------
// Panic available
// ---------------------------------------------------------------------------

void test_bb_diag_boot_serialize_panic_available(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason      = "panic",
        .wdt_resets        = 3,
        .panic_available   = true,
        .panic_boots_since = 2,
        .pending_verify    = false,
        .rolled_back       = false,
    };
    char *str = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NOT_NULL(strstr(str, "\"reset_reason\":\"panic\""));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"wdt_resets\":3"));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"panic\":"));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"available\":true"));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"boots_since\":2"));
    // flat panic_available key must NOT appear
    TEST_ASSERT_NULL(strstr(str, "\"panic_available\""));
    bb_json_free_str(str);
}

// ---------------------------------------------------------------------------
// rolled_back=true
// ---------------------------------------------------------------------------

void test_bb_diag_boot_serialize_rolled_back(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason    = "software",
        .wdt_resets      = 0,
        .panic_available = false,
        .panic_boots_since = 0,
        .pending_verify  = false,
        .rolled_back     = true,
    };
    char *str = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NOT_NULL(strstr(str, "\"rolled_back\":true"));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"pending_verify\":false"));
    bb_json_free_str(str);
}

// ---------------------------------------------------------------------------
// pending_verify=true
// ---------------------------------------------------------------------------

void test_bb_diag_boot_serialize_pending_verify(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason    = "software",
        .wdt_resets      = 0,
        .panic_available = false,
        .panic_boots_since = 0,
        .pending_verify  = true,
        .rolled_back     = false,
    };
    char *str = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NOT_NULL(strstr(str, "\"pending_verify\":true"));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"rolled_back\":false"));
    bb_json_free_str(str);
}

// ---------------------------------------------------------------------------
// panic object always present (available=false case has no boots_since)
// ---------------------------------------------------------------------------

void test_bb_diag_boot_serialize_panic_obj_always_present(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason    = "poweron",
        .wdt_resets      = 0,
        .panic_available = false,
        .panic_boots_since = 0,
        .pending_verify  = false,
        .rolled_back     = false,
    };
    char *str = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(str);
    // "panic":{...} must be present
    TEST_ASSERT_NOT_NULL(strstr(str, "\"panic\":"));
    // available key must be present inside panic
    TEST_ASSERT_NOT_NULL(strstr(str, "\"available\":"));
    // boots_since must NOT appear when panic unavailable
    TEST_ASSERT_NULL(strstr(str, "\"boots_since\""));
    bb_json_free_str(str);
}

// ---------------------------------------------------------------------------
// Starts with '{' and ends with '}'
// ---------------------------------------------------------------------------

void test_bb_diag_boot_serialize_json_braces(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason    = "poweron",
        .wdt_resets      = 0,
        .panic_available = false,
        .panic_boots_since = 0,
        .pending_verify  = false,
        .rolled_back     = false,
    };
    char *str = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_EQUAL_CHAR('{', str[0]);
    TEST_ASSERT_EQUAL_CHAR('}', str[strlen(str) - 1]);
    bb_json_free_str(str);
}

// ---------------------------------------------------------------------------
// Large wdt_resets (UINT32_MAX)
// ---------------------------------------------------------------------------

void test_bb_diag_boot_serialize_large_wdt_resets(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason    = "task_wdt",
        .wdt_resets      = 4294967295U,
        .panic_available = false,
        .panic_boots_since = 0,
        .pending_verify  = false,
        .rolled_back     = false,
    };
    char *str = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NOT_NULL(strstr(str, "\"reset_reason\":\"task_wdt\""));
    TEST_ASSERT_NOT_NULL(strstr(str, "4294967295"));
    bb_json_free_str(str);
}

// ---------------------------------------------------------------------------
// OOM: bb_json_obj_new returns NULL for the inner panic object
// Serializer must still emit all flat fields; panic key is absent.
// ---------------------------------------------------------------------------

void test_bb_diag_boot_serialize_panic_oom(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason    = "poweron",
        .wdt_resets      = 1,
        .panic_available = false,
        .panic_boots_since = 0,
        .pending_verify  = true,
        .rolled_back     = false,
    };

    // Call 0: bb_json_obj_new() in serialize_snap (outer object) — succeeds.
    // Call 1: bb_json_obj_new() for panic inside bb_diag_boot_serialize — fails.
    bb_json_host_force_alloc_fail_after(1);
    char *str = serialize_snap(&snap);
    bb_json_host_force_alloc_fail_after(-1); // reset guard

    TEST_ASSERT_NOT_NULL(str);
    // flat fields must still be present
    TEST_ASSERT_NOT_NULL(strstr(str, "\"reset_reason\":\"poweron\""));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"wdt_resets\":1"));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"pending_verify\":true"));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"rolled_back\":false"));
    // panic key must be absent when allocation failed
    TEST_ASSERT_NULL(strstr(str, "\"panic\""));
    bb_json_free_str(str);
}

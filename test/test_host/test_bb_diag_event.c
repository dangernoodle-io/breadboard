// Host unit tests for bb_diag diag.boot serializer:
// - bb_diag_boot_serialize (bb_cache serializer, nested shape)
#include "unity.h"
#include "bb_diag_event_priv.h"
#include "bb_json.h"
#include "bb_json_test_hooks.h"
#include "bb_system.h"

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
        .reset_reason    = "power-on",
        .wdt_resets      = 0,
        .panic_available = false,
        .panic_boots_since = 0,
        .pending_verify  = false,
        .rolled_back     = false,
    };
    char *str = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NOT_NULL(strstr(str, "\"reset_reason\":\"power-on\""));
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
        .reset_reason    = "power-on",
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
        .reset_reason    = "power-on",
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
        .reset_reason    = "power-on",
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
    TEST_ASSERT_NOT_NULL(strstr(str, "\"reset_reason\":\"power-on\""));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"wdt_resets\":1"));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"pending_verify\":true"));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"rolled_back\":false"));
    // panic key must be absent when allocation failed
    TEST_ASSERT_NULL(strstr(str, "\"panic\""));
    bb_json_free_str(str);
}

// ---------------------------------------------------------------------------
// reboot_reason (B1-527 PR-A) — always present; source/detail/epoch_s/age_s
// branches.
// ---------------------------------------------------------------------------

void test_bb_diag_boot_serialize_reboot_reason_unknown_default(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason = "power-on",
    };
    char *str = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NOT_NULL(strstr(str, "\"reboot_reason\":"));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"source\":\"unknown\""));
    // detail omitted when empty
    TEST_ASSERT_NULL(strstr(str, "\"detail\""));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"uptime_s\":0"));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"epoch_s\":0"));
    // age_s omitted when epoch_s == 0
    TEST_ASSERT_NULL(strstr(str, "\"age_s\""));
    bb_json_free_str(str);
}

void test_bb_diag_boot_serialize_reboot_reason_known_source_with_detail(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason  = "software",
        .reboot_src    = (uint8_t)BB_RESET_SRC_EGRESS_TIER3,
        .reboot_detail = "gw unreachable",
        .reboot_uptime_s = 3600,
    };
    char *str = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NOT_NULL(strstr(str, "\"source\":\"egress_tier3\""));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"detail\":\"gw unreachable\""));
    TEST_ASSERT_NOT_NULL(strstr(str, "\"uptime_s\":3600"));
    bb_json_free_str(str);
}

void test_bb_diag_boot_serialize_reboot_reason_epoch_no_age_when_now_invalid(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason     = "software",
        .reboot_src       = (uint8_t)BB_RESET_SRC_API_REBOOT,
        .reboot_epoch_s   = 1735689600U,
        .now_epoch_valid  = false,
        .now_epoch_s      = 1735689700U,
    };
    char *str = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NOT_NULL(strstr(str, "\"epoch_s\":1735689600"));
    // age_s omitted — now_epoch_valid is false (current clock not synced)
    TEST_ASSERT_NULL(strstr(str, "\"age_s\""));
    bb_json_free_str(str);
}

void test_bb_diag_boot_serialize_reboot_reason_age_present_when_both_valid(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason     = "software",
        .reboot_src       = (uint8_t)BB_RESET_SRC_API_REBOOT,
        .reboot_epoch_s   = 1735689600U,
        .now_epoch_valid  = true,
        .now_epoch_s      = 1735689700U,
    };
    char *str = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NOT_NULL(strstr(str, "\"age_s\":100"));
    bb_json_free_str(str);
}

void test_bb_diag_boot_serialize_reboot_reason_no_age_on_clock_skew(void)
{
    // now_epoch_s < reboot_epoch_s — defensive guard against a negative age.
    bb_diag_boot_snap_t snap = {
        .reset_reason     = "software",
        .reboot_src       = (uint8_t)BB_RESET_SRC_API_REBOOT,
        .reboot_epoch_s   = 1735689700U,
        .now_epoch_valid  = true,
        .now_epoch_s      = 1735689600U,
    };
    char *str = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NULL(strstr(str, "\"age_s\""));
    bb_json_free_str(str);
}

void test_bb_diag_boot_serialize_reboot_reason_oom(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason  = "power-on",
        .reboot_src    = (uint8_t)BB_RESET_SRC_FACTORY_RESET,
    };

    // Call 0: outer object (serialize_snap) — succeeds.
    // Call 1: panic object — succeeds.
    // Call 2: reboot_reason object — fails.
    bb_json_host_force_alloc_fail_after(2);
    char *str = serialize_snap(&snap);
    bb_json_host_force_alloc_fail_after(-1); // reset guard

    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NOT_NULL(strstr(str, "\"reset_reason\":\"power-on\""));
    // reboot_reason key absent when allocation failed
    TEST_ASSERT_NULL(strstr(str, "\"reboot_reason\""));
    bb_json_free_str(str);
}

#include "unity.h"
#include "bb_settings.h"

#include <string.h>

// bb_settings_nv_overlay_entries() is a pure schema projection over this
// component's own bb_config_field_t literals (B1-708 PR7) -- no storage
// access, so these tests need no fake backend registration, unlike
// test_bb_settings.c's accessor tests.

void test_bb_settings_nv_overlay_entries_returns_known_count(void)
{
    bb_settings_nv_overlay_entry_t entries[BB_SETTINGS_NV_OVERLAY_CAP];
    size_t total = bb_settings_nv_overlay_entries(entries, BB_SETTINGS_NV_OVERLAY_CAP);
    TEST_ASSERT_EQUAL_UINT(BB_SETTINGS_NV_OVERLAY_CAP, total);
}

void test_bb_settings_nv_overlay_entries_wifi_pass_is_secret(void)
{
    bb_settings_nv_overlay_entry_t entries[BB_SETTINGS_NV_OVERLAY_CAP];
    bb_settings_nv_overlay_entries(entries, BB_SETTINGS_NV_OVERLAY_CAP);

    bool found = false;
    for (size_t i = 0; i < BB_SETTINGS_NV_OVERLAY_CAP; i++) {
        if (strcmp(entries[i].key, "wifi_pass") == 0) {
            found = true;
            TEST_ASSERT_EQUAL_STRING("bb_cfg", entries[i].ns_or_dir);
            TEST_ASSERT_EQUAL_STRING("str", entries[i].type_str);
            TEST_ASSERT_EQUAL_STRING("WiFi Password", entries[i].label);
            TEST_ASSERT_TRUE(entries[i].secret);
            TEST_ASSERT_TRUE(entries[i].provisioning_only);
            TEST_ASSERT_TRUE(entries[i].reboot_required);
        }
    }
    TEST_ASSERT_TRUE(found);
}

void test_bb_settings_nv_overlay_entries_provisioned_is_not_secret(void)
{
    bb_settings_nv_overlay_entry_t entries[BB_SETTINGS_NV_OVERLAY_CAP];
    bb_settings_nv_overlay_entries(entries, BB_SETTINGS_NV_OVERLAY_CAP);

    bool found = false;
    for (size_t i = 0; i < BB_SETTINGS_NV_OVERLAY_CAP; i++) {
        if (strcmp(entries[i].key, "provisioned") == 0) {
            found = true;
            TEST_ASSERT_EQUAL_STRING("bb_cfg", entries[i].ns_or_dir);
            TEST_ASSERT_EQUAL_STRING("bool", entries[i].type_str);
            TEST_ASSERT_FALSE(entries[i].secret);
            TEST_ASSERT_FALSE(entries[i].provisioning_only);
            TEST_ASSERT_TRUE(entries[i].reboot_required);
        }
    }
    TEST_ASSERT_TRUE(found);
}

// Truncation-report contract: return value is the TOTAL count found even
// when it exceeds cap -- matches bb_storage_list_entries' contract.
void test_bb_settings_nv_overlay_entries_truncates_at_small_cap(void)
{
    bb_settings_nv_overlay_entry_t entries[1];
    size_t total = bb_settings_nv_overlay_entries(entries, 1);
    TEST_ASSERT_EQUAL_UINT(BB_SETTINGS_NV_OVERLAY_CAP, total);
    // Only the first entry was filled -- still a valid, non-NULL schema row.
    TEST_ASSERT_NOT_NULL(entries[0].key);
}

void test_bb_settings_nv_overlay_entries_cap_zero_still_reports_total(void)
{
    size_t total = bb_settings_nv_overlay_entries(NULL, 0);
    TEST_ASSERT_EQUAL_UINT(BB_SETTINGS_NV_OVERLAY_CAP, total);
}

#include "unity.h"
#include "bb_nv.h"
#include "bb_manifest.h"
#include "bb_json.h"
#include <string.h>

void test_nv_config_init_success(void)
{
    bb_err_t err = bb_nv_config_init();
    TEST_ASSERT_EQUAL_INT(0, err);
}

void test_nv_config_wifi_ssid_empty_after_init(void)
{
    bb_nv_config_init();
    const char *ssid = bb_nv_config_wifi_ssid();
    TEST_ASSERT_NOT_NULL(ssid);
    TEST_ASSERT_EQUAL_STRING("", ssid);
}

void test_nv_config_wifi_pass_empty_after_init(void)
{
    bb_nv_config_init();
    const char *pass = bb_nv_config_wifi_pass();
    TEST_ASSERT_NOT_NULL(pass);
    TEST_ASSERT_EQUAL_STRING("", pass);
}

void test_nv_config_display_enabled_default_true(void)
{
    bb_nv_config_init();
    bool enabled = bb_nv_config_display_enabled();
    TEST_ASSERT_TRUE(enabled);
}

void test_nv_config_mdns_enabled_default_true(void)
{
    bb_nv_config_init();
    bool enabled = bb_nv_config_mdns_enabled();
    TEST_ASSERT_TRUE(enabled);
}

void test_nv_config_is_provisioned_stub_returns_false(void)
{
    bb_nv_config_init();
    bool provisioned = bb_nv_config_is_provisioned();
    TEST_ASSERT_FALSE(provisioned);
}

void test_nv_config_init_registers_bb_cfg_keys(void)
{
    bb_manifest_clear();
    bb_err_t err = bb_nv_config_init();
    TEST_ASSERT_EQUAL(BB_OK, err);
    err = bb_nv_config_manifest_init();
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NOT_NULL(doc);

    char *json = bb_json_serialize(doc);
    TEST_ASSERT_NOT_NULL(json);

    // Namespace present
    TEST_ASSERT_NOT_NULL(strstr(json, "\"namespace\":\"bb_cfg\""));
    // Remaining keys present (hostname migrated to bb_settings, B1-754 --
    // it no longer appears in bb_nv's manifest).
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"wifi_ssid\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"wifi_pass\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"mdns_en\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"update_check_en\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"display_en\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"provisioned\""));

    bb_json_free_str(json);
    bb_json_free(doc);
    bb_manifest_clear();
}

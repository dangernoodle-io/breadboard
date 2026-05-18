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

void test_hostname_default_empty(void)
{
    bb_nv_config_init();
    const char *hostname = bb_nv_config_hostname();
    TEST_ASSERT_NOT_NULL(hostname);
    TEST_ASSERT_EQUAL_STRING("", hostname);
}

void test_hostname_set_get_roundtrip(void)
{
    bb_nv_config_init();
    bb_err_t err = bb_nv_config_set_hostname("tdongle-s3-1");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    const char *hostname = bb_nv_config_hostname();
    TEST_ASSERT_EQUAL_STRING("tdongle-s3-1", hostname);
}

void test_hostname_validates_charset(void)
{
    bb_nv_config_init();
    bb_err_t err = bb_nv_config_set_hostname("bad host!");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_hostname_validates_length(void)
{
    bb_nv_config_init();
    // 33 character string exceeds max of 32
    bb_err_t err = bb_nv_config_set_hostname("012345678901234567890123456789012");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_hostname_rejects_leading_hyphen(void)
{
    bb_nv_config_init();
    bb_err_t err = bb_nv_config_set_hostname("-foo");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_hostname_rejects_trailing_hyphen(void)
{
    bb_nv_config_init();
    bb_err_t err = bb_nv_config_set_hostname("foo-");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_hostname_rejects_null(void)
{
    bb_nv_config_init();
    bb_err_t err = bb_nv_config_set_hostname(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
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
    // All 6 keys present
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"wifi_ssid\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"wifi_pass\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"hostname\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"mdns_en\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"update_check_en\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"display_en\""));

    bb_json_free_str(json);
    bb_json_free(doc);
    bb_manifest_clear();
}

// Lazy-init test: bb_manifest_register_nv works without any explicit
// bb_manifest_init() call first — the registry is zero-initialized static
// state, always ready.
void test_nv_config_manifest_init_succeeds_before_manifest_init(void)
{
    // Simulate fresh boot: manifest registry is clean, bb_manifest_init
    // (HTTP route registration) has NOT been called.
    bb_manifest_clear();

    bb_err_t err = bb_nv_config_manifest_init();
    TEST_ASSERT_EQUAL_MESSAGE(BB_OK, err,
        "bb_manifest_register_nv must work before bb_manifest_init runs");

    // Verify keys are actually registered and visible
    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NOT_NULL(doc);
    char *json = bb_json_serialize(doc);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"namespace\":\"bb_cfg\""));
    bb_json_free_str(json);
    bb_json_free(doc);

    bb_manifest_clear();
}

// Duplicate-namespace regression for TA-380: bb_registry_init() walks
// PRE_HTTP internally, so if the caller already ran bb_registry_init_pre_http()
// explicitly, bb_nv_config_manifest_init() is called twice. With the old
// BB_ERR_INVALID_STATE return this aborted bb_registry_init() and panicked
// the device. The fix downgrades duplicates to warn+BB_OK so the double-walk
// is survivable while remaining visible in device logs.
void test_nv_config_manifest_init_double_call_returns_ok(void)
{
    bb_manifest_clear();

    bb_err_t first = bb_nv_config_manifest_init();
    TEST_ASSERT_EQUAL(BB_OK, first);

    // Second call (simulates double-walk via bb_registry_init_pre_http +
    // bb_registry_init) must NOT abort — must return BB_OK.
    bb_err_t second = bb_nv_config_manifest_init();
    TEST_ASSERT_EQUAL_MESSAGE(BB_OK, second,
        "duplicate bb_cfg registration must return BB_OK (double-walk tolerated)");

    bb_manifest_clear();
}

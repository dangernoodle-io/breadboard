#include "unity.h"
#include "bb_settings.h"
#include "bb_manifest.h"
#include "bb_json.h"
#include "bb_storage.h"
#include "fake_nvs_backend.h"
#include <string.h>

// Moved+renamed (B1-963/B1-708) from test_nv_config.c's
// test_nv_config_init_success/_wifi_ssid_empty_after_init/
// _wifi_pass_empty_after_init/_init_registers_bb_cfg_keys -- bb_settings no
// longer forwards to bb_nv_config_init/_manifest_init, it OWNS the shell +
// manifest registration now. bb_settings_creds_boot_init's host build is a
// pure no-op (its whole body is #ifdef ESP_PLATFORM, byte-identical to
// bb_nv_config_init's prior host stub) -- "empty after init" means: with no
// creds ever written, bb_settings' own accessors report none -- register the
// fake "nvs" backend the same way test_bb_settings.c does, for isolation
// from whatever backend state a prior test in the suite left registered.
static void reset_storage(void)
{
    bb_storage_test_reset();
    fake_nvs_reset();
    bb_storage_register_backend("nvs", &s_fake_nvs_vtable, NULL);
}

void test_bb_settings_creds_boot_init_success(void)
{
    bb_err_t err = bb_settings_creds_boot_init();
    TEST_ASSERT_EQUAL_INT(0, err);
}

void test_bb_settings_creds_boot_wifi_ssid_empty_after_init(void)
{
    reset_storage();
    bb_settings_creds_boot_init();
    TEST_ASSERT_FALSE(bb_settings_wifi_has_creds());
}

void test_bb_settings_creds_boot_wifi_pass_empty_after_init(void)
{
    reset_storage();
    bb_settings_creds_boot_init();
    // The live wifi.pass field has no has_default -- an unset key propagates
    // bb_storage's own BB_ERR_NOT_FOUND (bb_settings_wifi_has_creds is the
    // accessor with fail-closed-to-"no creds" semantics; a raw get here
    // surfaces the underlying not-found instead of masking it).
    char pass[70] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_settings_wifi_pass_get(pass, sizeof(pass), &len));
}

void test_bb_settings_creds_boot_init_registers_bb_cfg_keys(void)
{
    bb_manifest_clear();
    bb_err_t err = bb_settings_creds_boot_init();
    TEST_ASSERT_EQUAL(BB_OK, err);
    err = bb_settings_creds_boot_manifest_init();
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NOT_NULL(doc);

    char *json = bb_json_serialize(doc);
    TEST_ASSERT_NOT_NULL(json);

    // Namespace present
    TEST_ASSERT_NOT_NULL(strstr(json, "\"namespace\":\"bb_cfg\""));
    // Remaining keys present (hostname/timezone migrated to bb_settings,
    // B1-754/B1-750 -- they no longer appear in this manifest; likewise
    // mdns_en/update_check_en/display_en, B1-750 -- their /api/manifest
    // registration is a known, accepted gap, B1-936).
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"wifi_ssid\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"wifi_pass\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"provisioned\""));
    TEST_ASSERT_NULL(strstr(json, "\"key\":\"mdns_en\""));
    TEST_ASSERT_NULL(strstr(json, "\"key\":\"update_check_en\""));
    TEST_ASSERT_NULL(strstr(json, "\"key\":\"display_en\""));

    bb_json_free_str(json);
    bb_json_free(doc);
    bb_manifest_clear();
}

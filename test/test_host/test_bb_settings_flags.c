// B1-750 — display_enabled/mdns_enabled/update_check_enabled moved from
// bb_nv to bb_settings (bb_nv dissolution epic B1-708); bb_settings'
// accessors forward to bb_config_get_bool/set_bool over the SAME "bb_cfg"
// NVS namespace + "display_en"/"mdns_en"/"update_check_en" keys bb_nv used
// (see platform/host/bb_settings/bb_settings.c's
// BB_SETTINGS_DISPLAY_EN_KEY/BB_SETTINGS_MDNS_EN_KEY/
// BB_SETTINGS_UPDATE_CHECK_EN_KEY comment) -- byte-compat with
// already-provisioned boards.
//
// Tests per flag:
//   1. default-true when unset
//   2. set/get round trip
//   3. byte-compat: the legacy "bb_cfg"/<key> address is the SAME one
//      bb_settings_<flag>_get/set read/write (literal-address BITE test)
//   4. fail-open to true on a real backend I/O error too, not just
//      not-found (mirrors bb_nv_config_init's own
//      "nvs_get_u8(...) != ESP_OK -> default 1" fallback)

#include "unity.h"
#include "bb_settings.h"
#include "bb_storage.h"
#include "bb_config.h"
#include "fake_nvs_backend.h"

#include <string.h>

// Test-local field descriptors targeting the exact same addr (backend/ns/
// key) as bb_settings.c's internal s_display_enabled_field/
// s_mdns_enabled_field/s_update_check_enabled_field -- the literal legacy
// address bb_nv_config used, kept hardcoded here (NOT via bb_settings' own
// field descriptors) so the byte-compat contract is executable, mirroring
// test_bb_ntp_timezone.c's pattern.
static const bb_config_field_t s_test_display_en_field = {
    .id   = "display.enabled",
    .type = BB_CONFIG_BOOL,
    .addr = { .backend = "nvs", .ns_or_dir = "bb_cfg", .key = "display_en" },
};
static const bb_config_field_t s_test_mdns_en_field = {
    .id   = "mdns.enabled",
    .type = BB_CONFIG_BOOL,
    .addr = { .backend = "nvs", .ns_or_dir = "bb_cfg", .key = "mdns_en" },
};
static const bb_config_field_t s_test_update_check_en_field = {
    .id   = "update_check.enabled",
    .type = BB_CONFIG_BOOL,
    .addr = { .backend = "nvs", .ns_or_dir = "bb_cfg", .key = "update_check_en" },
};

static void reset_state(void)
{
    bb_storage_test_reset();
    fake_nvs_reset();
    bb_storage_register_backend("nvs", &s_fake_nvs_vtable, NULL);
}

// ---------------------------------------------------------------------------
// display_enabled
// ---------------------------------------------------------------------------

void test_settings_display_enabled_default_true(void)
{
    reset_state();
    TEST_ASSERT_TRUE(bb_settings_display_enabled_get());
}

void test_settings_display_enabled_set_round_trip(void)
{
    reset_state();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_display_enabled_set(false));
    TEST_ASSERT_FALSE(bb_settings_display_enabled_get());
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_display_enabled_set(true));
    TEST_ASSERT_TRUE(bb_settings_display_enabled_get());
}

void test_settings_display_enabled_byte_compat_legacy_address_readable_via_accessor(void)
{
    reset_state();
    // Seed directly at the literal legacy ns/key (bypassing bb_settings.c's
    // own field descriptor entirely).
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_bool(&s_test_display_en_field, false));
    TEST_ASSERT_FALSE(bb_settings_display_enabled_get());
}

void test_settings_display_enabled_byte_compat_accessor_write_readable_via_legacy_address(void)
{
    reset_state();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_display_enabled_set(false));
    bool v = true;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_bool(&s_test_display_en_field, &v));
    TEST_ASSERT_FALSE(v);
}

void test_settings_display_enabled_fails_open_on_backend_error(void)
{
    reset_state();
    // A real, non-not-found backend error on get() must resolve to the
    // default (true), same as an unset key -- mirrors bb_nv_config_init's
    // "nvs_get_u8(...) != ESP_OK -> default 1" fallback exactly.
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_display_enabled_set(false));
    fake_nvs_backend_fail_key("display_en");
    TEST_ASSERT_TRUE(bb_settings_display_enabled_get());
}

// ---------------------------------------------------------------------------
// mdns_enabled
// ---------------------------------------------------------------------------

void test_settings_mdns_enabled_default_true(void)
{
    reset_state();
    TEST_ASSERT_TRUE(bb_settings_mdns_enabled_get());
}

void test_settings_mdns_enabled_set_round_trip(void)
{
    reset_state();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_mdns_enabled_set(false));
    TEST_ASSERT_FALSE(bb_settings_mdns_enabled_get());
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_mdns_enabled_set(true));
    TEST_ASSERT_TRUE(bb_settings_mdns_enabled_get());
}

void test_settings_mdns_enabled_byte_compat_legacy_address_readable_via_accessor(void)
{
    reset_state();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_bool(&s_test_mdns_en_field, false));
    TEST_ASSERT_FALSE(bb_settings_mdns_enabled_get());
}

void test_settings_mdns_enabled_byte_compat_accessor_write_readable_via_legacy_address(void)
{
    reset_state();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_mdns_enabled_set(false));
    bool v = true;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_bool(&s_test_mdns_en_field, &v));
    TEST_ASSERT_FALSE(v);
}

void test_settings_mdns_enabled_fails_open_on_backend_error(void)
{
    reset_state();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_mdns_enabled_set(false));
    fake_nvs_backend_fail_key("mdns_en");
    TEST_ASSERT_TRUE(bb_settings_mdns_enabled_get());
}

// ---------------------------------------------------------------------------
// update_check_enabled
// ---------------------------------------------------------------------------

void test_settings_update_check_enabled_default_true(void)
{
    reset_state();
    TEST_ASSERT_TRUE(bb_settings_update_check_enabled_get());
}

void test_settings_update_check_enabled_set_round_trip(void)
{
    reset_state();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_update_check_enabled_set(false));
    TEST_ASSERT_FALSE(bb_settings_update_check_enabled_get());
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_update_check_enabled_set(true));
    TEST_ASSERT_TRUE(bb_settings_update_check_enabled_get());
}

void test_settings_update_check_enabled_byte_compat_legacy_address_readable_via_accessor(void)
{
    reset_state();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_bool(&s_test_update_check_en_field, false));
    TEST_ASSERT_FALSE(bb_settings_update_check_enabled_get());
}

void test_settings_update_check_enabled_byte_compat_accessor_write_readable_via_legacy_address(void)
{
    reset_state();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_update_check_enabled_set(false));
    bool v = true;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_bool(&s_test_update_check_en_field, &v));
    TEST_ASSERT_FALSE(v);
}

void test_settings_update_check_enabled_fails_open_on_backend_error(void)
{
    reset_state();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_update_check_enabled_set(false));
    fake_nvs_backend_fail_key("update_check_en");
    TEST_ASSERT_TRUE(bb_settings_update_check_enabled_get());
}

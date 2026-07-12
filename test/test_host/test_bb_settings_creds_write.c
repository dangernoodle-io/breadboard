#include "unity.h"
#include "bb_settings.h"
#include "bb_storage.h"
#include "bb_storage_rtc.h"
#include "bb_config.h"
#include "fake_nvs_backend.h"

#include <string.h>

// bb_settings_wifi_set (live-creds writer, B1 PR4) stages through
// bb_config_staged -> bb_storage_txn_* against backend="nvs" (same mechanism
// as test_bb_settings_wifi_pending.c -- register the shared fake in-memory
// vtable WITH the txn group under "nvs"). The RTC warm-reboot mirror is
// exercised against the REAL bb_storage_rtc host backend (bb_storage_rtc_
// register()/bb_storage_rtc_test_reset(), same pattern as
// test_bb_storage_rtc.c) -- no second fake needed, bb_storage_rtc already
// has a host implementation.
//
// Test-local field descriptors target the exact same addr (backend/ns/key)
// as bb_settings.c's internal s_wifi_ssid_field/s_wifi_pass_field, mirroring
// test_bb_settings.c's own pattern.

static const bb_config_field_t s_test_ssid_field = {
    .id = "wifi.ssid", .type = BB_CONFIG_STR,
    .addr = { .backend = "nvs", .ns_or_dir = "bb_cfg", .key = "wifi_ssid" }, .max_len = 32,
};
static const bb_config_field_t s_test_pass_field = {
    .id = "wifi.pass", .type = BB_CONFIG_STR,
    .addr = { .backend = "nvs", .ns_or_dir = "bb_cfg", .key = "wifi_pass" }, .max_len = 64,
};
static const bb_config_field_t s_test_ssid_p_field = {
    .id = "wifi.ssid_pending", .type = BB_CONFIG_STR,
    .addr = { .backend = "nvs", .ns_or_dir = "bb_cfg", .key = "wifi_ssid_p" }, .max_len = 32,
};
static const bb_config_field_t s_test_pass_p_field = {
    .id = "wifi.pass_pending", .type = BB_CONFIG_STR,
    .addr = { .backend = "nvs", .ns_or_dir = "bb_cfg", .key = "wifi_pass_p" }, .max_len = 64,
};
static const bb_config_field_t s_test_try_field = {
    .id = "wifi.try_pending", .type = BB_CONFIG_U8,
    .addr = { .backend = "nvs", .ns_or_dir = "bb_cfg", .key = "wifi_try" },
};

static bb_storage_addr_t rtc_addr(const char *key)
{
    bb_storage_addr_t addr = { .backend = "rtc", .ns_or_dir = NULL, .key = key };
    return addr;
}

// Resets NVS (fake) + registers it, and resets the RTC region WITHOUT
// registering the "rtc" backend -- individual tests opt into RTC
// registration so the fail-open (unregistered backend) case is easy to
// exercise as the default.
static void reset_all(void)
{
    bb_storage_test_reset();
    fake_nvs_reset();
    bb_storage_register_backend("nvs", &s_fake_nvs_vtable, NULL);
    bb_storage_rtc_test_reset();
}

static void register_rtc(void)
{
    bb_storage_rtc_register();
}

/* ---------------------------------------------------------------------------
 * wifi_set: round trip through the live accessors, NULL pass -> empty.
 * ---------------------------------------------------------------------------*/
void test_bb_settings_wifi_set_round_trip(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_set("MyNetwork", "hunter2"));

    char ssid[40] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_ssid_get(ssid, sizeof(ssid), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("MyNetwork", ssid, len);

    char pass[70] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pass_get(pass, sizeof(pass), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("hunter2", pass, len);

    TEST_ASSERT_TRUE(bb_settings_wifi_has_creds());
}

void test_bb_settings_wifi_set_null_pass_treated_as_empty(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_set("MyNetwork", NULL));

    char pass[70] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pass_get(pass, sizeof(pass), &len));
    TEST_ASSERT_EQUAL(0, len);
}

// ssid/pass NULL asymmetry (bb_settings.h): NULL pass -> "" (open network),
// but NULL ssid is NOT substituted -- bb_config_staged_set_str poisons the
// session on a NULL v and the commit surfaces BB_ERR_INVALID_ARG, WITHOUT
// touching the live NVS fields or the RTC mirror.
void test_bb_settings_wifi_set_null_ssid_returns_invalid_arg(void)
{
    reset_all();
    register_rtc();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_settings_wifi_set(NULL, "hunter2"));

    TEST_ASSERT_FALSE(bb_config_exists(&s_test_ssid_field));
    TEST_ASSERT_FALSE(bb_config_exists(&s_test_pass_field));

    bb_storage_addr_t ssid_addr = rtc_addr("ssid");
    bb_storage_addr_t pass_addr = rtc_addr("pass");
    bb_storage_addr_t prov_addr = rtc_addr("provisioned");
    TEST_ASSERT_FALSE(bb_storage_exists(&ssid_addr));
    TEST_ASSERT_FALSE(bb_storage_exists(&pass_addr));
    TEST_ASSERT_FALSE(bb_storage_exists(&prov_addr));
}

/* ---------------------------------------------------------------------------
 * wifi_set: RTC mirror -- reflects on success (registered), fail-open when
 * unregistered, untouched on an overflow precheck failure.
 * ---------------------------------------------------------------------------*/
void test_bb_settings_wifi_set_mirrors_rtc_backend(void)
{
    reset_all();
    register_rtc();

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_set("MyNetwork", "hunter2"));

    char ssid[32] = {0};
    size_t len = 0;
    bb_storage_addr_t ssid_addr = rtc_addr("ssid");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&ssid_addr, ssid, sizeof(ssid), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("MyNetwork", ssid, len);

    char pass[64] = {0};
    bb_storage_addr_t pass_addr = rtc_addr("pass");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&pass_addr, pass, sizeof(pass), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("hunter2", pass, len);

    uint8_t provisioned = 0;
    bb_storage_addr_t prov_addr = rtc_addr("provisioned");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&prov_addr, &provisioned, sizeof(provisioned), &len));
    TEST_ASSERT_EQUAL_UINT8(1, provisioned);
}

// No "rtc" backend registered at all -- bb_settings_wifi_set must still
// return BB_OK (fail-open) and the live NVS creds must still be correct.
void test_bb_settings_wifi_set_ok_when_rtc_backend_unregistered(void)
{
    reset_all();
    // Deliberately NOT calling register_rtc().

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_set("MyNetwork", "hunter2"));

    char ssid[40] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_ssid_get(ssid, sizeof(ssid), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("MyNetwork", ssid, len);
}

// Overflow: the staged precheck fails BEFORE the atomic commit, so neither
// the live NVS fields nor the RTC mirror are touched.
void test_bb_settings_wifi_set_overflow_returns_invalid_arg_leaves_rtc_untouched(void)
{
    reset_all();
    register_rtc();

    char long_ssid[40];
    memset(long_ssid, 'A', sizeof(long_ssid) - 1);
    long_ssid[sizeof(long_ssid) - 1] = '\0';

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_settings_wifi_set(long_ssid, "hunter2"));

    TEST_ASSERT_FALSE(bb_config_exists(&s_test_ssid_field));
    TEST_ASSERT_FALSE(bb_config_exists(&s_test_pass_field));

    bb_storage_addr_t ssid_addr = rtc_addr("ssid");
    bb_storage_addr_t pass_addr = rtc_addr("pass");
    bb_storage_addr_t prov_addr = rtc_addr("provisioned");
    TEST_ASSERT_FALSE(bb_storage_exists(&ssid_addr));
    TEST_ASSERT_FALSE(bb_storage_exists(&pass_addr));
    TEST_ASSERT_FALSE(bb_storage_exists(&prov_addr));
}

/* ---------------------------------------------------------------------------
 * promote: now also mirrors to RTC on success, fail-open when unregistered.
 * ---------------------------------------------------------------------------*/
void test_bb_settings_wifi_pending_promote_mirrors_rtc_backend(void)
{
    reset_all();
    register_rtc();

    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_p_field, "PromotedNet"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_pass_p_field, "s3cr3t!"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_u8(&s_test_try_field, 1));

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_promote());

    char ssid[32] = {0};
    size_t len = 0;
    bb_storage_addr_t ssid_addr = rtc_addr("ssid");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&ssid_addr, ssid, sizeof(ssid), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("PromotedNet", ssid, len);

    char pass[64] = {0};
    bb_storage_addr_t pass_addr = rtc_addr("pass");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&pass_addr, pass, sizeof(pass), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("s3cr3t!", pass, len);

    uint8_t provisioned = 0;
    bb_storage_addr_t prov_addr = rtc_addr("provisioned");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&prov_addr, &provisioned, sizeof(provisioned), &len));
    TEST_ASSERT_EQUAL_UINT8(1, provisioned);
}

// A stale mirror from a prior write must be OVERWRITTEN by promote, not left
// stale -- the mirror always reflects the just-promoted values.
void test_bb_settings_wifi_pending_promote_mirror_not_stale(void)
{
    reset_all();
    register_rtc();

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_set("OldNet", "oldpass"));

    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_p_field, "NewNet"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_pass_p_field, "newpass"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_u8(&s_test_try_field, 1));

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_promote());

    char ssid[32] = {0};
    size_t len = 0;
    bb_storage_addr_t ssid_addr = rtc_addr("ssid");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&ssid_addr, ssid, sizeof(ssid), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("NewNet", ssid, len);
}

void test_bb_settings_wifi_pending_promote_ok_when_rtc_backend_unregistered(void)
{
    reset_all();
    // Deliberately NOT calling register_rtc().

    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_p_field, "PromotedNet"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_pass_p_field, "s3cr3t!"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_u8(&s_test_try_field, 1));

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_promote());

    char ssid[40] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_str(&s_test_ssid_field, ssid, sizeof(ssid), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("PromotedNet", ssid, len);
}

/* ---------------------------------------------------------------------------
 * wifi_set and promote are INDEPENDENT live-writers converging on the same
 * live fields -- neither calls the other.
 * ---------------------------------------------------------------------------*/
void test_bb_settings_wifi_set_and_promote_converge_on_same_live_fields(void)
{
    reset_all();
    register_rtc();

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_set("DirectNet", "directpass"));

    char ssid[40] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_ssid_get(ssid, sizeof(ssid), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("DirectNet", ssid, len);

    // promote() is independently callable and overwrites the same live
    // fields wifi_set() just wrote -- no coupling between the two writers.
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_p_field, "PendingNet"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_pass_p_field, "pendingpass"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_u8(&s_test_try_field, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_promote());

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_ssid_get(ssid, sizeof(ssid), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("PendingNet", ssid, len);
}

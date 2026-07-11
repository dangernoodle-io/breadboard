#include "unity.h"
#include "bb_settings.h"
#include "bb_storage.h"
#include "bb_config.h"
#include "fake_nvs_backend.h"

#include <string.h>

// bb_settings' direct WiFi-credential accessors forward to bb_config field
// accessors targeting backend="nvs" (the same ns/keys bb_nv_config uses --
// see bb_settings.c). The real "nvs" bb_storage backend is ESP-IDF-only, so
// these host tests register the shared fake in-memory vtable (see
// fake_nvs_backend.h) under the name "nvs" -- exercising the SAME production
// field table/addr code path bb_settings.c uses, with only the backend's
// storage swapped for a host-safe stand-in (mirrors test_bb_storage_typed.c's
// fake_get/fake_set pattern).
//
// The wifi-creds accessors have no setter API -- seeding test creds goes
// through a TEST-LOCAL bb_config_field_t pointing at the exact same addr
// (backend/ns/key) as bb_settings.c's internal fields, so a seeded value is
// visible through the real accessors. hostname (B1-754) DOES have a real
// setter (bb_settings_hostname_set) -- its tests use that directly.

// Test-local field descriptors targeting the exact same addr (backend/ns/
// key) as bb_settings.c's internal s_wifi_ssid_field/s_wifi_pass_field, used
// only to seed state the real accessors read.
static const bb_config_field_t s_test_ssid_field = {
    .id      = "wifi.ssid",
    .type    = BB_CONFIG_STR,
    .addr    = { .backend = "nvs", .ns_or_dir = "bb_cfg", .key = "wifi_ssid" },
    .max_len = 32,
};

static const bb_config_field_t s_test_pass_field = {
    .id      = "wifi.pass",
    .type    = BB_CONFIG_STR,
    .addr    = { .backend = "nvs", .ns_or_dir = "bb_cfg", .key = "wifi_pass" },
    .max_len = 64,
};

static void reset_all(void)
{
    bb_storage_test_reset();
    fake_nvs_reset();
    bb_storage_register_backend("nvs", &s_fake_nvs_vtable, NULL);
}

/* ---------------------------------------------------------------------------
 * ssid_get / pass_get round trip
 * ---------------------------------------------------------------------------*/
void test_bb_settings_wifi_ssid_get_pass_get_round_trip(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_field, "MyNetwork"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_pass_field, "hunter2"));

    char ssid[40] = {0};
    size_t ssid_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_ssid_get(ssid, sizeof(ssid), &ssid_len));
    TEST_ASSERT_EQUAL(strlen("MyNetwork"), ssid_len);
    TEST_ASSERT_EQUAL_STRING_LEN("MyNetwork", ssid, ssid_len);

    char pass[70] = {0};
    size_t pass_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pass_get(pass, sizeof(pass), &pass_len));
    TEST_ASSERT_EQUAL(strlen("hunter2"), pass_len);
    TEST_ASSERT_EQUAL_STRING_LEN("hunter2", pass, pass_len);
}

/* ---------------------------------------------------------------------------
 * has_creds
 * ---------------------------------------------------------------------------*/
void test_bb_settings_wifi_has_creds_false_when_unset(void)
{
    reset_all();
    TEST_ASSERT_FALSE(bb_settings_wifi_has_creds());
}

void test_bb_settings_wifi_has_creds_true_after_set(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_field, "MyNetwork"));

    TEST_ASSERT_TRUE(bb_settings_wifi_has_creds());
}

// bb_settings_wifi_has_creds uses NON-EMPTY-VALUE semantics (matches bb_wifi's
// fallback wifi_has_creds(), which checks ssid[0]) -- NOT mere key
// presence. An empty-but-present ssid key must report false, or the
// bb_settings and fallback paths drift for a board that persisted an empty
// string (e.g. a cleared-but-not-erased field).
void test_bb_settings_wifi_has_creds_false_when_value_empty(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_field, ""));
    TEST_ASSERT_TRUE(bb_config_exists(&s_test_ssid_field)); // key IS present

    TEST_ASSERT_FALSE(bb_settings_wifi_has_creds());
}

/* ---------------------------------------------------------------------------
 * cap=0 size-probe + truncation contract (mirrors bb_config_get_str)
 * ---------------------------------------------------------------------------*/
void test_bb_settings_wifi_ssid_get_cap_zero_probes_length(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_field, "MyNetwork"));

    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_ssid_get(NULL, 0, &out_len));
    TEST_ASSERT_EQUAL(strlen("MyNetwork"), out_len);
}

void test_bb_settings_wifi_pass_get_truncation_reports_full_len(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_pass_field, "hunter2222"));

    char buf[4] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pass_get(buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(strlen("hunter2222"), out_len);
    TEST_ASSERT_TRUE(out_len > sizeof(buf));
    TEST_ASSERT_EQUAL_STRING_LEN("hunt", buf, sizeof(buf));
}

/* ---------------------------------------------------------------------------
 * NULL out_len (#776 CRITICAL) -- a caller that doesn't need the length back
 * must still get buf correctly filled. This is the load-bearing branch a
 * NULL out_len passed all the way to bb_config_get_str would have silently
 * skipped (bb_config_get_str rejects NULL out_len with BB_ERR_INVALID_ARG,
 * leaving buf untouched -- empty SSID/pass at connect time).
 * ---------------------------------------------------------------------------*/
void test_bb_settings_wifi_ssid_get_null_out_len_still_fills_buf(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_field, "MyNetwork"));

    char ssid[40] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_ssid_get(ssid, sizeof(ssid), NULL));
    TEST_ASSERT_EQUAL_STRING("MyNetwork", ssid);
}

void test_bb_settings_wifi_pass_get_null_out_len_still_fills_buf(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_pass_field, "hunter2"));

    char pass[70] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pass_get(pass, sizeof(pass), NULL));
    TEST_ASSERT_EQUAL_STRING("hunter2", pass);
}

/* ---------------------------------------------------------------------------
 * hostname (B1-754 -- migrated from bb_nv's bb_nv_config_hostname/
 * bb_nv_config_set_hostname). Unlike the wifi-creds fields, hostname has a
 * real setter (bb_settings_hostname_set) that validates before persisting.
 * ---------------------------------------------------------------------------*/
void test_hostname_default_empty(void)
{
    reset_all();

    char hn[40] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_hostname_get(hn, sizeof(hn), &len));
    TEST_ASSERT_EQUAL(0, len);
    TEST_ASSERT_EQUAL_STRING("", hn);
}

void test_hostname_set_get_roundtrip(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_hostname_set("tdongle-s3-1"));

    char hn[40] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_hostname_get(hn, sizeof(hn), &len));
    TEST_ASSERT_EQUAL(strlen("tdongle-s3-1"), len);
    TEST_ASSERT_EQUAL_STRING("tdongle-s3-1", hn);
}

// Boundary: exactly 32 chars (the max valid length) must round-trip
// successfully -- max_len is buffer CAPACITY (33: 32 usable + NUL), so a
// full-length 32-char hostname must not be rejected (regression guard for
// an off-by-one that previously set max_len=32, one byte short).
void test_hostname_set_get_roundtrip_32_char_boundary(void)
{
    reset_all();
    const char *hn32 = "01234567890123456789012345678901"; // 32 chars
    TEST_ASSERT_EQUAL(32, strlen(hn32));
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_hostname_set(hn32));

    char hn[40] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_hostname_get(hn, sizeof(hn), &len));
    TEST_ASSERT_EQUAL(32, len);
    TEST_ASSERT_EQUAL_STRING(hn32, hn);
}

void test_hostname_set_rejects_null(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_settings_hostname_set(NULL));
}

void test_hostname_set_rejects_empty(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_settings_hostname_set(""));
}

void test_hostname_set_rejects_too_long(void)
{
    reset_all();
    // 33 characters exceeds the 32-char max.
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_settings_hostname_set("012345678901234567890123456789012"));
}

void test_hostname_set_rejects_leading_hyphen(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_settings_hostname_set("-foo"));
}

void test_hostname_set_rejects_trailing_hyphen(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_settings_hostname_set("foo-"));
}

void test_hostname_set_rejects_bad_charset(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_settings_hostname_set("bad host!"));
}

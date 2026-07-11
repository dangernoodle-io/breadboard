#include "unity.h"
#include "bb_settings.h"
#include "bb_storage.h"
#include "bb_config.h"
#include "fake_nvs_backend.h"

#include <string.h>

// bb_settings' WiFi pending-creds writers stage through bb_config_staged ->
// bb_storage_txn_* against backend="nvs" (the same ns/keys bb_nv's pending
// path uses -- see bb_settings.c). The real "nvs" bb_storage backend is
// ESP-IDF-only, so these host tests register the shared fake in-memory
// vtable (see fake_nvs_backend.h, WITH the txn group) under the name "nvs"
// -- the SAME fake mechanism test_bb_settings.c registers. No second
// mechanism.
//
// The pending-creds writers have no way to directly poke the fake store, so
// seeding/asserting state goes through TEST-LOCAL bb_config_field_t
// descriptors pointing at the exact same addr (backend/ns/key) as
// bb_settings.c's internal s_wifi_ssid_p_field/s_wifi_pass_p_field/
// s_wifi_try_field/s_wifi_ssid_field/s_wifi_pass_field, mirroring
// test_bb_settings.c's own s_test_ssid_field/s_test_pass_field pattern.

// Test-local field descriptors targeting the exact same addr (backend/ns/
// key) as bb_settings.c's internal fields, used to seed/assert state the
// pending-creds writers read/write.
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

static void reset_all(void)
{
    bb_storage_test_reset();
    fake_nvs_reset();
    bb_storage_register_backend("nvs", &s_fake_nvs_vtable, NULL);
}

/* ---------------------------------------------------------------------------
 * pending_set: stages all 3 keys atomically.
 * ---------------------------------------------------------------------------*/
void test_bb_settings_wifi_pending_set_stages_ssid_pass_try(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_set("MyNetwork", "hunter2"));

    char ssid[40] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_ssid_get(ssid, sizeof(ssid), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("MyNetwork", ssid, len);

    char pass[70] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_pass_get(pass, sizeof(pass), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("hunter2", pass, len);

    uint8_t try_flag = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u8(&s_test_try_field, &try_flag));
    TEST_ASSERT_EQUAL(1, try_flag);
}

void test_bb_settings_wifi_pending_set_null_pass_treated_as_empty(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_set("MyNetwork", NULL));

    char pass[70] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_pass_get(pass, sizeof(pass), &len));
    TEST_ASSERT_EQUAL(0, len);
}

// A mid-stage failure (NULL ssid poisons the staged session's local sticky
// error) must land nothing at all -- all-or-nothing through bb_settings, not
// just partially staged.
void test_bb_settings_wifi_pending_set_null_ssid_lands_nothing(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_settings_wifi_pending_set(NULL, "hunter2"));

    TEST_ASSERT_FALSE(bb_config_exists(&s_test_ssid_p_field));
    TEST_ASSERT_FALSE(bb_config_exists(&s_test_pass_p_field));
    TEST_ASSERT_FALSE(bb_config_exists(&s_test_try_field));
}

/* ---------------------------------------------------------------------------
 * pending_active: try!=0 AND ssid non-empty (mirrors bb_wifi_pending_decide's
 * gate, exercised through the storage path).
 * ---------------------------------------------------------------------------*/
void test_bb_settings_wifi_pending_active_false_when_unset(void)
{
    reset_all();
    TEST_ASSERT_FALSE(bb_settings_wifi_pending_active());
}

void test_bb_settings_wifi_pending_active_true_after_set(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_set("MyNetwork", "hunter2"));
    TEST_ASSERT_TRUE(bb_settings_wifi_pending_active());
}

void test_bb_settings_wifi_pending_active_false_when_try_zero(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_p_field, "MyNetwork"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_u8(&s_test_try_field, 0));
    TEST_ASSERT_FALSE(bb_settings_wifi_pending_active());
}

void test_bb_settings_wifi_pending_active_false_when_ssid_empty(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_p_field, ""));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_u8(&s_test_try_field, 1));
    TEST_ASSERT_FALSE(bb_settings_wifi_pending_active());
}

/* ---------------------------------------------------------------------------
 * pending_promote: live = former pending, try=0, atomic; guard when no
 * pending ssid staged; pending bytes erased afterward (best-effort cleanup
 * ran).
 * ---------------------------------------------------------------------------*/
void test_bb_settings_wifi_pending_promote_no_pending_returns_invalid_state(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_settings_wifi_pending_promote());
}

// Backend error on the ssid_p read must abort promote -- treated the same
// as "no pending" (fail-closed, see bb_settings_wifi_pending_active).
// Nothing lands on the live keys.
void test_bb_settings_wifi_pending_promote_ssid_read_error_aborts(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_set("MyNetwork", "hunter2"));

    fake_nvs_backend_fail_key("wifi_ssid_p");
    TEST_ASSERT_NOT_EQUAL(BB_OK, bb_settings_wifi_pending_promote());

    TEST_ASSERT_FALSE(bb_config_exists(&s_test_ssid_field));
    TEST_ASSERT_FALSE(bb_config_exists(&s_test_pass_field));
}

// Regression guard for the HIGH finding: a backend error on the pass_p read
// must ALSO abort promote -- not silently commit an empty live pass over a
// (still-promoted) live ssid. FAILS against the pre-fix code (which swallowed
// this error and committed anyway), PASSES post-fix (perr propagated,
// nothing committed).
void test_bb_settings_wifi_pending_promote_pass_read_error_aborts(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_set("MyNetwork", "hunter2"));

    fake_nvs_backend_fail_key("wifi_pass_p");
    TEST_ASSERT_NOT_EQUAL(BB_OK, bb_settings_wifi_pending_promote());

    TEST_ASSERT_FALSE(bb_config_exists(&s_test_ssid_field));
    TEST_ASSERT_FALSE(bb_config_exists(&s_test_pass_field));
}

void test_bb_settings_wifi_pending_promote_moves_pending_to_live_and_clears_try(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_set("MyNetwork", "hunter2"));

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_promote());

    char ssid[40] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_str(&s_test_ssid_field, ssid, sizeof(ssid), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("MyNetwork", ssid, len);

    char pass[70] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_str(&s_test_pass_field, pass, sizeof(pass), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("hunter2", pass, len);

    uint8_t try_flag = 1;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u8(&s_test_try_field, &try_flag));
    TEST_ASSERT_EQUAL(0, try_flag);
}

// The best-effort cleanup ran: pending bytes are erased after a successful
// promote.
void test_bb_settings_wifi_pending_promote_erases_pending_bytes(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_set("MyNetwork", "hunter2"));
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_promote());

    TEST_ASSERT_FALSE(bb_config_exists(&s_test_ssid_p_field));
    TEST_ASSERT_FALSE(bb_config_exists(&s_test_pass_p_field));
}

// Crash-safety intent: try=0 is the decision bit, independent of whether the
// best-effort erase ran. After a successful promote, pending_active reads
// false purely because try is 0 -- even considered in isolation from the
// (already-verified) erased pending bytes above.
void test_bb_settings_wifi_pending_promote_leaves_pending_active_false(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_set("MyNetwork", "hunter2"));
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_promote());

    TEST_ASSERT_FALSE(bb_settings_wifi_pending_active());
}

/* ---------------------------------------------------------------------------
 * pending_clear: try=0, idempotent, pending bytes erased.
 * ---------------------------------------------------------------------------*/
void test_bb_settings_wifi_pending_clear_clears_try_and_erases_bytes(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_set("MyNetwork", "hunter2"));

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_clear());

    uint8_t try_flag = 1;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u8(&s_test_try_field, &try_flag));
    TEST_ASSERT_EQUAL(0, try_flag);
    TEST_ASSERT_FALSE(bb_config_exists(&s_test_ssid_p_field));
    TEST_ASSERT_FALSE(bb_config_exists(&s_test_pass_p_field));
    TEST_ASSERT_FALSE(bb_settings_wifi_pending_active());
}

void test_bb_settings_wifi_pending_clear_idempotent_when_never_set(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_clear());
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_clear());
    TEST_ASSERT_FALSE(bb_settings_wifi_pending_active());
}

/* ---------------------------------------------------------------------------
 * pending getters: size-probe, truncation, NULL out_len, empty-on-unset.
 * ---------------------------------------------------------------------------*/
void test_bb_settings_wifi_pending_ssid_get_empty_when_unset(void)
{
    reset_all();
    char ssid[40] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_ssid_get(ssid, sizeof(ssid), &len));
    TEST_ASSERT_EQUAL(0, len);
    TEST_ASSERT_EQUAL_STRING("", ssid);
}

void test_bb_settings_wifi_pending_ssid_get_cap_zero_probes_length(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_p_field, "MyNetwork"));

    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_ssid_get(NULL, 0, &out_len));
    TEST_ASSERT_EQUAL(strlen("MyNetwork"), out_len);
}

void test_bb_settings_wifi_pending_pass_get_truncation_reports_full_len(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_pass_p_field, "hunter2222"));

    char buf[4] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_pass_get(buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(strlen("hunter2222"), out_len);
    TEST_ASSERT_TRUE(out_len > sizeof(buf));
    TEST_ASSERT_EQUAL_STRING_LEN("hunt", buf, sizeof(buf));
}

void test_bb_settings_wifi_pending_ssid_get_null_out_len_still_fills_buf(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_p_field, "MyNetwork"));

    char ssid[40] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_ssid_get(ssid, sizeof(ssid), NULL));
    TEST_ASSERT_EQUAL_STRING("MyNetwork", ssid);
}

void test_bb_settings_wifi_pending_pass_get_null_out_len_still_fills_buf(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_pass_p_field, "hunter2"));

    char pass[70] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_pass_get(pass, sizeof(pass), NULL));
    TEST_ASSERT_EQUAL_STRING("hunter2", pass);
}

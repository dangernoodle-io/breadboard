#include "unity.h"
#include "bb_settings.h"
#include "bb_storage.h"
#include "bb_storage_rtc.h"
#include "bb_config.h"
#include "fake_nvs_backend.h"
#include "bb_nv_creds_boot_decide.h"

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

/* ---------------------------------------------------------------------------
 * B1-763: the RTC mirror is now a SINGLE atomic bb_config_staged commit --
 * all 3 keys (ssid/pass/provisioned) land together against the "rtc"
 * backend, observable via the same facade a real consumer would use.
 * ---------------------------------------------------------------------------*/
void test_bb_settings_wifi_set_mirrors_all_3_rtc_keys_atomically(void)
{
    reset_all();
    register_rtc();

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_set("AtomicNet", "atomicpass"));

    char ssid[32] = {0};
    size_t len = 0;
    bb_storage_addr_t ssid_addr = rtc_addr("ssid");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&ssid_addr, ssid, sizeof(ssid), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("AtomicNet", ssid, len);

    char pass[64] = {0};
    bb_storage_addr_t pass_addr = rtc_addr("pass");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&pass_addr, pass, sizeof(pass), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("atomicpass", pass, len);

    uint8_t provisioned = 0;
    bb_storage_addr_t prov_addr = rtc_addr("provisioned");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&prov_addr, &provisioned, sizeof(provisioned), &len));
    TEST_ASSERT_EQUAL_UINT8(1, provisioned);
}

/* ---------------------------------------------------------------------------
 * bb_nv creds-cluster relocation: RTC mirror accessors
 * (bb_settings_wifi_rtc_mirror_has_creds/_ssid_get/_pass_get/
 * _provisioned_get/_clear) -- bb_nv's heal/seed/factory-reset call these
 * directly instead of owning a private RTC region. The heal/seed CALL SITES
 * themselves live in platform/espidf/bb_nv/bb_nv.c (ESP_PLATFORM-only, not
 * host-compiled/coverage-visible -- B1-943/B1-516); these tests instead
 * bite-proof the PRIMITIVES those call sites are built from, at the exact
 * same public bb_settings surface bb_nv.c uses.
 * ---------------------------------------------------------------------------*/
void test_bb_settings_wifi_rtc_mirror_has_creds_false_when_unregistered(void)
{
    reset_all();
    // Deliberately NOT calling register_rtc().
    TEST_ASSERT_FALSE(bb_settings_wifi_rtc_mirror_has_creds());
}

void test_bb_settings_wifi_rtc_mirror_has_creds_false_when_empty(void)
{
    reset_all();
    register_rtc();
    TEST_ASSERT_FALSE(bb_settings_wifi_rtc_mirror_has_creds());
}

// The exact "should the seed fire" gate bb_nv_config_init's mirror-seed
// evaluates: has_creds() flips true only AFTER a write lands, proving the
// gate this test exercises is the SAME storage state a seed call would
// observe. Reverting bb_settings_wifi_rtc_mirror_write's provisioned=1 stage
// (or has_creds' bb_storage_exists wrap) turns this RED.
void test_bb_settings_wifi_rtc_mirror_has_creds_true_after_write(void)
{
    reset_all();
    register_rtc();

    TEST_ASSERT_FALSE(bb_settings_wifi_rtc_mirror_has_creds());
    bb_settings_wifi_rtc_mirror_write("SeedNet", "seedpass");
    TEST_ASSERT_TRUE(bb_settings_wifi_rtc_mirror_has_creds());
}

// Public bb_settings_wifi_rtc_mirror_write wrapper (bb_nv's mirror-seed/
// provisioned-repack call this directly): round-trips through the SAME 3
// mirror accessors bb_nv's heal reads on the next boot.
void test_bb_settings_wifi_rtc_mirror_write_round_trips_via_accessors(void)
{
    reset_all();
    register_rtc();

    bb_settings_wifi_rtc_mirror_write("SeedNet", "seedpass");

    char ssid[32] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_rtc_mirror_ssid_get(ssid, sizeof(ssid), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("SeedNet", ssid, len);

    char pass[64] = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_rtc_mirror_pass_get(pass, sizeof(pass), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("seedpass", pass, len);

    TEST_ASSERT_TRUE(bb_settings_wifi_rtc_mirror_provisioned_get());
}

// NULL pass is treated as empty (open network) -- same asymmetry as
// bb_settings_wifi_set's NULL-pass contract.
void test_bb_settings_wifi_rtc_mirror_write_null_pass_treated_as_empty(void)
{
    reset_all();
    register_rtc();

    bb_settings_wifi_rtc_mirror_write("SeedNet", NULL);

    char pass[64] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_rtc_mirror_pass_get(pass, sizeof(pass), &len));
    TEST_ASSERT_EQUAL(0, len);
}

// Fail-open: no "rtc" backend registered -- the write is a silent no-op,
// never a crash/error surfaced to the (void-returning) caller.
void test_bb_settings_wifi_rtc_mirror_write_ok_when_rtc_backend_unregistered(void)
{
    reset_all();
    // Deliberately NOT calling register_rtc().
    bb_settings_wifi_rtc_mirror_write("SeedNet", "seedpass");
    TEST_ASSERT_FALSE(bb_settings_wifi_rtc_mirror_has_creds());
}

// Getters propagate BB_ERR_NOT_FOUND (not BB_OK+empty) when the mirror is
// empty/unregistered -- neither mirror field carries a has_default, matching
// the LIVE wifi.ssid/wifi.pass fields' own contract (see
// bb_settings_wifi_ssid_get). Callers gate on has_creds() first.
void test_bb_settings_wifi_rtc_mirror_ssid_get_empty_when_unregistered(void)
{
    reset_all();
    // Deliberately NOT calling register_rtc().
    char ssid[32] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_settings_wifi_rtc_mirror_ssid_get(ssid, sizeof(ssid), &len));
}

// Fail-CLOSED (opposite direction from display/mdns/update-check's fail-
// open-to-true default): an unregistered/empty mirror must read
// provisioned=false, never true -- a heal must never mistakenly re-mark a
// recovered board as provisioned off a storage error.
void test_bb_settings_wifi_rtc_mirror_provisioned_get_false_when_unregistered(void)
{
    reset_all();
    // Deliberately NOT calling register_rtc().
    TEST_ASSERT_FALSE(bb_settings_wifi_rtc_mirror_provisioned_get());
}

// Whole-region invalidate: clear() zeroes the mirror so has_creds/ssid_get/
// pass_get/provisioned_get all revert to "nothing here" -- the ROLLBACK
// bb_nv's factory-reset/clear_provisioned/clear_wifi paths depend on to stop
// a heal from resurrecting cleared creds. Reverting
// bb_settings_wifi_rtc_mirror_clear() to a no-op (e.g. `return BB_OK;`
// without the bb_storage_erase call) turns this RED.
void test_bb_settings_wifi_rtc_mirror_clear_invalidates_region(void)
{
    reset_all();
    register_rtc();

    bb_settings_wifi_rtc_mirror_write("ClearNet", "clearpass");
    TEST_ASSERT_TRUE(bb_settings_wifi_rtc_mirror_has_creds());
    TEST_ASSERT_TRUE(bb_settings_wifi_rtc_mirror_provisioned_get());

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_rtc_mirror_clear());

    TEST_ASSERT_FALSE(bb_settings_wifi_rtc_mirror_has_creds());
    TEST_ASSERT_FALSE(bb_settings_wifi_rtc_mirror_provisioned_get());

    char ssid[32] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_settings_wifi_rtc_mirror_ssid_get(ssid, sizeof(ssid), &len));
}

// clear() on an unregistered "rtc" backend surfaces bb_storage_erase's own
// error (BB_ERR_NOT_FOUND) -- unlike the write paths, this wrapper does NOT
// swallow the error; callers (bb_nv's factory-reset et al.) choose fail-open
// for themselves by ignoring the return, same posture as clear_provisioned/
// clear_wifi.
void test_bb_settings_wifi_rtc_mirror_clear_propagates_error_when_unregistered(void)
{
    reset_all();
    // Deliberately NOT calling register_rtc().
    TEST_ASSERT_NOT_EQUAL(BB_OK, bb_settings_wifi_rtc_mirror_clear());
}

/* ---------------------------------------------------------------------------
 * Seed-gate integration: bb_nv_config_init's heal-vs-seed policy is a pure
 * function, bb_nv_creds_boot_decide (components/bb_nv/src/, unit-tested
 * against all 4 boolean combinations in test_bb_nv_creds_boot_decide.c).
 * These tests prove the COMPOSITION: driving that same decision function
 * with the REAL bb_settings_wifi_has_creds() / bb_settings_wifi_rtc_mirror_
 * has_creds() readers, against real bb_settings/bb_storage_rtc state, yields
 * the correct action -- not just that the precondition readers report the
 * right booleans in isolation. The decision's CALL SITE in bb_nv_config_init
 * remains ESP_PLATFORM-only (see file header comment above); only the I/O
 * that follows the decision rides on HW validation.
 * ---------------------------------------------------------------------------*/
void test_bb_settings_wifi_rtc_mirror_seed_gate_true_when_live_creds_and_empty_mirror(void)
{
    reset_all();
    register_rtc();

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_set("LiveNet", "livepass"));
    // wifi_set() itself already mirrors on success -- clear it to reproduce
    // the specific pre-seed state (live creds present, mirror NOT yet
    // written) the seed's gate is meant to catch, e.g. a mirror that was
    // never armed on a factory-flashed board pre-dating this relocation.
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_rtc_mirror_clear());

    bb_nv_boot_action_t action = bb_nv_creds_boot_decide(bb_settings_wifi_has_creds(),
                                                          bb_settings_wifi_rtc_mirror_has_creds());
    TEST_ASSERT_EQUAL(BB_NV_BOOT_SEED, action);
}

void test_bb_settings_wifi_rtc_mirror_seed_gate_false_when_mirror_already_valid(void)
{
    reset_all();
    register_rtc();

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_set("LiveNet", "livepass"));
    // wifi_set()'s own best-effort mirror write already armed the mirror.

    bb_nv_boot_action_t action = bb_nv_creds_boot_decide(bb_settings_wifi_has_creds(),
                                                          bb_settings_wifi_rtc_mirror_has_creds());
    // A correctly-gated seed must NOT fire here, never clobbering an
    // already-valid mirror (which may carry in-flight pending-try state).
    TEST_ASSERT_EQUAL(BB_NV_BOOT_NONE, action);
}

void test_bb_settings_wifi_rtc_mirror_seed_gate_false_when_no_live_creds(void)
{
    reset_all();
    register_rtc();

    bb_nv_boot_action_t action = bb_nv_creds_boot_decide(bb_settings_wifi_has_creds(),
                                                          bb_settings_wifi_rtc_mirror_has_creds());
    // Nothing live to seed the mirror with -- must NOT fire.
    TEST_ASSERT_EQUAL(BB_NV_BOOT_NONE, action);
}

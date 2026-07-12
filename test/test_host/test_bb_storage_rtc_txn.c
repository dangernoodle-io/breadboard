#include "unity.h"
#include "bb_storage.h"
#include "bb_storage_rtc.h"
#include "bb_storage_rtc_region.h"
#include "bb_core.h"

#include <string.h>

// bb_storage_rtc's optional txn vtable group (B1-763): begin/set/commit/
// abort, driven through the FACADE (bb_storage_txn_begin("rtc", ...) etc.,
// not the _for_test direct-drive seam) so this exercises the exact path a
// real bb_config_staged consumer (bb_settings) takes. THE regression this
// component fixes: an abort (simulating a crash before commit) must leave
// the live region byte-identical to its pre-txn value -- never a torn mix
// of old and new fields.

static void reset_all(void)
{
    bb_storage_test_reset();
    bb_storage_rtc_test_reset();
    bb_storage_rtc_register();
}

/* ---------------------------------------------------------------------------
 * begin -> set x3 -> commit round-trip: all 3 keys land together.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_txn_begin_set_commit_round_trip(void)
{
    reset_all();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("rtc", "", &txn));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "ssid", BB_STORAGE_ENC_STR, "MyNet", 5));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "pass", BB_STORAGE_ENC_STR, "hunter2", 7));
    uint8_t provisioned = 1;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "provisioned", BB_STORAGE_ENC_U8,
                                                 &provisioned, sizeof(provisioned)));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_commit(&txn));

    bb_storage_addr_t ssid_addr = { .backend = "rtc", .ns_or_dir = NULL, .key = "ssid" };
    bb_storage_addr_t pass_addr = { .backend = "rtc", .ns_or_dir = NULL, .key = "pass" };
    bb_storage_addr_t prov_addr = { .backend = "rtc", .ns_or_dir = NULL, .key = "provisioned" };

    char buf[64] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&ssid_addr, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("MyNet", buf, len);
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&pass_addr, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("hunter2", buf, len);
    uint8_t prov_out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&prov_addr, &prov_out, sizeof(prov_out), &len));
    TEST_ASSERT_EQUAL_UINT8(1, prov_out);
}

/* ---------------------------------------------------------------------------
 * abort leaves the region byte-identical to its pre-txn state.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_txn_abort_leaves_region_untouched(void)
{
    reset_all();

    bb_storage_addr_t ssid_addr = { .backend = "rtc", .ns_or_dir = NULL, .key = "ssid" };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ssid_addr, "PreExisting", 11));

    bb_storage_rtc_region_t before = *bb_storage_rtc_region_for_test();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("rtc", "", &txn));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "pass", BB_STORAGE_ENC_STR, "newpass", 7));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_abort(&txn));

    bb_storage_rtc_region_t *after = bb_storage_rtc_region_for_test();
    TEST_ASSERT_EQUAL_MEMORY(&before, after, sizeof(before));
}

/* ---------------------------------------------------------------------------
 * THE B1-763 REGRESSION: a pre-seeded, fully-provisioned region (ssid=A,
 * pass=A, provisioned=1) with a staged rotation (ssid=B, pass=B) that is
 * ABORTED (simulating a crash before commit) must leave the region EXACTLY
 * at ssid=A/pass=A/provisioned=1 -- never a torn A/B mix.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_txn_abort_before_commit_never_leaves_torn_mix(void)
{
    reset_all();

    bb_storage_addr_t ssid_addr = { .backend = "rtc", .ns_or_dir = NULL, .key = "ssid" };
    bb_storage_addr_t pass_addr = { .backend = "rtc", .ns_or_dir = NULL, .key = "pass" };
    bb_storage_addr_t prov_addr = { .backend = "rtc", .ns_or_dir = NULL, .key = "provisioned" };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ssid_addr, "A", 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&pass_addr, "A", 1));
    uint8_t provisioned = 1;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&prov_addr, &provisioned, sizeof(provisioned)));

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("rtc", "", &txn));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "ssid", BB_STORAGE_ENC_STR, "B", 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "pass", BB_STORAGE_ENC_STR, "B", 1));
    // Simulate a crash before commit ever runs -- abort() is what a real
    // caller's cleanup path (or the next boot's fresh begin()) sees.
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_abort(&txn));

    char buf[64] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&ssid_addr, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("A", buf, len);
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&pass_addr, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("A", buf, len);
    uint8_t prov_out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&prov_addr, &prov_out, sizeof(prov_out), &len));
    TEST_ASSERT_EQUAL_UINT8(1, prov_out);
}

/* ---------------------------------------------------------------------------
 * Overflow in a staged set poisons the txn; commit returns the sticky error
 * and the live region is left untouched.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_txn_overflow_set_poisons_and_commit_returns_sticky_error(void)
{
    reset_all();

    bb_storage_addr_t ssid_addr = { .backend = "rtc", .ns_or_dir = NULL, .key = "ssid" };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ssid_addr, "Existing", 8));
    bb_storage_rtc_region_t before = *bb_storage_rtc_region_for_test();

    char long_ssid[40];
    memset(long_ssid, 'A', sizeof(long_ssid));

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("rtc", "", &txn));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                       bb_storage_txn_set(&txn, "ssid", BB_STORAGE_ENC_STR, long_ssid, sizeof(long_ssid)));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_storage_txn_commit(&txn));

    bb_storage_rtc_region_t *after = bb_storage_rtc_region_for_test();
    TEST_ASSERT_EQUAL_MEMORY(&before, after, sizeof(before));
}

/* ---------------------------------------------------------------------------
 * Txn on a cold/invalid region: staging ssid only, committing, leaves ssid
 * set and pass/provisioned zeroed (not garbage) -- mirrors rtc_set's own
 * invalid-region-promotes-to-zero rule.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_txn_on_cold_region_zeroes_unstaged_fields(void)
{
    reset_all();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("rtc", "", &txn));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "ssid", BB_STORAGE_ENC_STR, "ColdNet", 7));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_commit(&txn));

    bb_storage_addr_t ssid_addr = { .backend = "rtc", .ns_or_dir = NULL, .key = "ssid" };
    bb_storage_addr_t pass_addr = { .backend = "rtc", .ns_or_dir = NULL, .key = "pass" };
    bb_storage_addr_t prov_addr = { .backend = "rtc", .ns_or_dir = NULL, .key = "provisioned" };

    char buf[64] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&ssid_addr, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("ColdNet", buf, len);

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&pass_addr, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(0, len);

    uint8_t prov_out = 0xFF;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&prov_addr, &prov_out, sizeof(prov_out), &len));
    TEST_ASSERT_EQUAL_UINT8(0, prov_out);
}

/* ---------------------------------------------------------------------------
 * Partial-key txn (ssid+pass, no provisioned) on an already-valid,
 * provisioned region -- commit preserves the prior provisioned value.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_txn_partial_key_preserves_prior_provisioned(void)
{
    reset_all();

    bb_storage_addr_t ssid_addr = { .backend = "rtc", .ns_or_dir = NULL, .key = "ssid" };
    bb_storage_addr_t pass_addr = { .backend = "rtc", .ns_or_dir = NULL, .key = "pass" };
    bb_storage_addr_t prov_addr = { .backend = "rtc", .ns_or_dir = NULL, .key = "provisioned" };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ssid_addr, "OldNet", 6));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&pass_addr, "oldpass", 7));
    uint8_t provisioned = 1;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&prov_addr, &provisioned, sizeof(provisioned)));

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("rtc", "", &txn));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "ssid", BB_STORAGE_ENC_STR, "NewNet", 6));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "pass", BB_STORAGE_ENC_STR, "newpass", 7));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_commit(&txn));

    char buf[64] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&ssid_addr, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("NewNet", buf, len);
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&pass_addr, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("newpass", buf, len);

    uint8_t prov_out = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&prov_addr, &prov_out, sizeof(prov_out), &len));
    TEST_ASSERT_EQUAL_UINT8(1, prov_out);
}

/* ---------------------------------------------------------------------------
 * B1-763 follow-up: a duplicate set() for the same key within one open txn
 * overwrites the already-staged slot in place (last-write-wins) rather than
 * consuming a second one -- proves the shared bb_storage_txn_slot_stage()
 * match-existing-key path, not just a slot-count side effect.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_txn_duplicate_key_last_wins(void)
{
    reset_all();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("rtc", "", &txn));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "ssid", BB_STORAGE_ENC_STR, "First", 5));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "ssid", BB_STORAGE_ENC_STR, "Second", 6));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_commit(&txn));

    bb_storage_addr_t ssid_addr = { .backend = "rtc", .ns_or_dir = NULL, .key = "ssid" };
    char buf[64] = {0};
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&ssid_addr, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_STRING_LEN("Second", buf, len);
}

/* ---------------------------------------------------------------------------
 * B1-763 follow-up: an unknown key hits rtc_txn_classify's gate (the
 * backend-specific validation that stays in rtc, ahead of the shared slot
 * helper) -- returns BB_ERR_INVALID_ARG and poisons the txn; a subsequent
 * commit surfaces the sticky error WITHOUT ever touching s_region.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_txn_unknown_key_invalid_arg(void)
{
    reset_all();

    bb_storage_addr_t ssid_addr = { .backend = "rtc", .ns_or_dir = NULL, .key = "ssid" };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ssid_addr, "Existing", 8));
    bb_storage_rtc_region_t before = *bb_storage_rtc_region_for_test();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("rtc", "", &txn));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_storage_txn_set(&txn, "bogus", BB_STORAGE_ENC_STR, "x", 1));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_txn_commit(&txn));

    bb_storage_rtc_region_t *after = bb_storage_rtc_region_for_test();
    TEST_ASSERT_EQUAL_MEMORY(&before, after, sizeof(before));
}

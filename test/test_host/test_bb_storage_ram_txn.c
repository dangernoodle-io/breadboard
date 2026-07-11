#include "unity.h"
#include "bb_storage.h"
#include "bb_storage_ram.h"
#include "bb_core.h"

#include <stdio.h>
#include <string.h>

// bb_storage_txn_* against the real "ram" backend: atomicity (commit applies
// everything or nothing), rollback on abort, capacity overflow (slot table
// and per-value size), and begin-twice-without-close rejection.

static void reset_all(void)
{
    bb_storage_test_reset();
    bb_storage_ram_test_reset();
    bb_storage_ram_register();
}

static bb_storage_addr_t addr_for(const char *key)
{
    bb_storage_addr_t addr = { .backend = "ram", .ns_or_dir = NULL, .key = key };
    return addr;
}

/* ---------------------------------------------------------------------------
 * 1. begin -> set(a) -> set(b) -> commit -> both visible via bb_storage_get.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_ram_txn_commit_makes_both_keys_visible(void)
{
    reset_all();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("ram", "ns", &txn));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "a", BB_STORAGE_ENC_STR, "hello", 5));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "b", BB_STORAGE_ENC_STR, "world", 5));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_commit(&txn));

    char buf[16] = {0};
    size_t out_len = 0;
    bb_storage_addr_t a = addr_for("a");
    bb_storage_addr_t b = addr_for("b");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&a, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_STRING_LEN("hello", buf, out_len);
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&b, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_STRING_LEN("world", buf, out_len);
}

/* ---------------------------------------------------------------------------
 * 2. begin -> set(a) -> set(b) -> abort -> neither visible (rollback).
 * ---------------------------------------------------------------------------*/
void test_bb_storage_ram_txn_abort_leaves_neither_key_visible(void)
{
    reset_all();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("ram", "ns", &txn));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "a", BB_STORAGE_ENC_STR, "hello", 5));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "b", BB_STORAGE_ENC_STR, "world", 5));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_abort(&txn));

    bb_storage_addr_t a = addr_for("a");
    bb_storage_addr_t b = addr_for("b");
    TEST_ASSERT_FALSE(bb_storage_exists(&a));
    TEST_ASSERT_FALSE(bb_storage_exists(&b));
}

/* ---------------------------------------------------------------------------
 * 3. set x (MAX_KEYS+1) -> overflow set returns NO_SPACE, txn poisoned,
 *    commit returns it, no keys land.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_ram_txn_slot_overflow_poisons_txn_and_lands_nothing(void)
{
    reset_all();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("ram", "ns", &txn));

    char key[BB_STORAGE_TXN_KEY_MAX_BYTES];
    for (int i = 0; i < BB_STORAGE_TXN_MAX_KEYS; i++) {
        snprintf(key, sizeof(key), "k%d", i);
        TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, key, BB_STORAGE_ENC_STR, "v", 1));
    }
    /* one more than the table can hold */
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_storage_txn_set(&txn, "overflow", BB_STORAGE_ENC_STR, "v", 1));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_storage_txn_commit(&txn));

    for (int i = 0; i < BB_STORAGE_TXN_MAX_KEYS; i++) {
        snprintf(key, sizeof(key), "k%d", i);
        bb_storage_addr_t a = addr_for(key);
        TEST_ASSERT_FALSE(bb_storage_exists(&a));
    }
}

/* ---------------------------------------------------------------------------
 * 4. value > VALUE_MAX_BYTES -> NO_SPACE, no keys land.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_ram_txn_oversize_value_returns_no_space_and_lands_nothing(void)
{
    reset_all();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("ram", "ns", &txn));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "a", BB_STORAGE_ENC_STR, "hello", 5));

    uint8_t big[BB_STORAGE_TXN_VALUE_MAX_BYTES + 1];
    memset(big, 'x', sizeof(big));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_storage_txn_set(&txn, "b", BB_STORAGE_ENC_BLOB, big, sizeof(big)));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_storage_txn_commit(&txn));

    bb_storage_addr_t a = addr_for("a");
    TEST_ASSERT_FALSE(bb_storage_exists(&a));
}

/* ---------------------------------------------------------------------------
 * 5. Commit visibility: no partial-slot state observable — a successful
 *    commit that overwrites an existing key plus adds a new one leaves
 *    exactly the expected final state (lock-held apply is all-or-nothing
 *    by construction; this asserts the observable outcome).
 * ---------------------------------------------------------------------------*/
void test_bb_storage_ram_txn_commit_visibility_is_all_or_nothing(void)
{
    reset_all();

    /* pre-existing key outside any txn */
    bb_storage_addr_t existing = addr_for("existing");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&existing, "old", 3));

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("ram", "ns", &txn));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "existing", BB_STORAGE_ENC_STR, "new", 3));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "fresh", BB_STORAGE_ENC_STR, "val", 3));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_commit(&txn));

    char buf[8] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&existing, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_STRING_LEN("new", buf, out_len);

    bb_storage_addr_t fresh = addr_for("fresh");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&fresh, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_STRING_LEN("val", buf, out_len);
}

/* ---------------------------------------------------------------------------
 * 6. begin twice on same txn without commit/abort -> BB_ERR_INVALID_STATE.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_ram_txn_begin_twice_without_close_returns_invalid_state(void)
{
    reset_all();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("ram", "ns", &txn));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_storage_txn_begin("ram", "ns", &txn));

    /* clean up */
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_abort(&txn));
}

/* ---------------------------------------------------------------------------
 * 7. Real all-or-nothing precheck: fill the RAM backend's entry table (via
 *    plain bb_storage_set(), NOT the txn-local slot table exercised by test
 *    3 above) to BB_STORAGE_RAM_MAX_ENTRIES, then stage one NOT-yet-present
 *    key in a txn and commit. ram_txn_commit's new_keys_needed > free_count
 *    guard must reject the commit with BB_ERR_NO_SPACE BEFORE applying
 *    anything, leaving the backend's state completely unchanged.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_ram_txn_commit_precheck_rejects_when_backend_table_full(void)
{
    reset_all();

    char key[BB_STORAGE_RAM_MAX_KEY_BYTES];
    for (int i = 0; i < BB_STORAGE_RAM_MAX_ENTRIES; i++) {
        snprintf(key, sizeof(key), "existing%d", i);
        bb_storage_addr_t a = addr_for(key);
        TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&a, "v", 1));
    }

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("ram", "ns", &txn));
    /* "brand-new" is not among the existing0..N-1 keys above, so committing
     * it needs one more table slot than free_count (0) provides. */
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "brand-new", BB_STORAGE_ENC_STR, "x", 1));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_storage_txn_commit(&txn));

    bb_storage_addr_t brand_new = addr_for("brand-new");
    TEST_ASSERT_FALSE(bb_storage_exists(&brand_new));

    for (int i = 0; i < BB_STORAGE_RAM_MAX_ENTRIES; i++) {
        snprintf(key, sizeof(key), "existing%d", i);
        bb_storage_addr_t a = addr_for(key);
        char buf[4] = {0};
        size_t out_len = 0;
        TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&a, buf, sizeof(buf), &out_len));
        TEST_ASSERT_EQUAL_STRING_LEN("v", buf, out_len);
    }
}

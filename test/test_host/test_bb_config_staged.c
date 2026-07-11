#include "unity.h"
#include "bb_config_staged.h"
#include "bb_config.h"
#include "bb_storage.h"
#include "bb_storage_ram.h"
#include "bb_core.h"

#include <string.h>

// bb_config_staged against the real "ram" backend (the only backend that
// implements the bb_storage txn group in host tests -- see
// test_bb_storage_ram_txn.c). Every field here is declared with
// addr.ns_or_dir == the session's ns_or_dir ("ns") -- staging requires a
// homogeneous backend/ns per session (see F_CROSS_NS/F_CROSS_BACKEND below
// for the deliberate mismatch cases).

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------*/
static void reset_all(void)
{
    bb_storage_test_reset();
    bb_storage_ram_test_reset();
    bb_storage_ram_register();
}

#define ADDR(k) { .backend = "ram", .ns_or_dir = "ns", .key = (k) }
#define ADDR_OTHER_NS(k) { .backend = "ram", .ns_or_dir = "other-ns", .key = (k) }
#define ADDR_OTHER_BACKEND(k) { .backend = "other-backend", .ns_or_dir = "ns", .key = (k) }

static const bb_config_field_t F_BOOL = {
    .id = "test.bool", .type = BB_CONFIG_BOOL, .addr = ADDR("bool"),
};
static const bb_config_field_t F_U8 = {
    .id = "test.u8", .type = BB_CONFIG_U8, .addr = ADDR("u8"),
};
static const bb_config_field_t F_U16 = {
    .id = "test.u16", .type = BB_CONFIG_U16, .addr = ADDR("u16"),
};
static const bb_config_field_t F_U32 = {
    .id = "test.u32", .type = BB_CONFIG_U32, .addr = ADDR("u32"),
};
static const bb_config_field_t F_I32 = {
    .id = "test.i32", .type = BB_CONFIG_I32, .addr = ADDR("i32"),
};
static const bb_config_field_t F_STR = {
    .id = "test.str", .type = BB_CONFIG_STR, .addr = ADDR("str"), .max_len = 8,
};
static const bb_config_field_t F_BLOB = {
    .id = "test.blob", .type = BB_CONFIG_BLOB, .addr = ADDR("blob"), .max_len = 8,
};
// max_len bigger than the txn transport cap (BB_STORAGE_TXN_VALUE_MAX_BYTES,
// default 64) so a value can pass this layer's local precheck yet still be
// rejected by the wrapped txn -- the delegated (txn-level) NO_SPACE path.
static const bb_config_field_t F_BLOB_BIG = {
    .id = "test.blob_big", .type = BB_CONFIG_BLOB, .addr = ADDR("blob_big"), .max_len = 128,
};
static const bb_config_field_t F_CROSS_NS = {
    .id = "test.cross_ns", .type = BB_CONFIG_U8, .addr = ADDR_OTHER_NS("cross_ns"),
};
static const bb_config_field_t F_CROSS_BACKEND = {
    .id = "test.cross_backend", .type = BB_CONFIG_U8, .addr = ADDR_OTHER_BACKEND("cross_backend"),
};

/* ---------------------------------------------------------------------------
 * 1. Stage a mix of fields (<= BB_STORAGE_TXN_MAX_KEYS) -> commit -> every
 *    value reads back via bb_config_get_*.
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_stage_and_commit_lands_all_fields(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_bool(&h, &F_BOOL, true));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_u8(&h, &F_U8, 7));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_str(&h, &F_STR, "hi"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_commit(&h));

    bool b = false;
    uint8_t u8 = 0;
    char buf[16] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_bool(&F_BOOL, &b));
    TEST_ASSERT_TRUE(b);
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u8(&F_U8, &u8));
    TEST_ASSERT_EQUAL(7, u8);
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_str(&F_STR, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_STRING_LEN("hi", buf, out_len);
}

/* ---------------------------------------------------------------------------
 * 1b. u16/u32/i32 round trip (one session, all three -- exactly MAX_KEYS).
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_scalar_round_trip_u16_u32_i32(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_u16(&h, &F_U16, 4321));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_u32(&h, &F_U32, 0xDEADBEEFu));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_i32(&h, &F_I32, -12345));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_commit(&h));

    uint16_t u16 = 0;
    uint32_t u32 = 0;
    int32_t i32 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u16(&F_U16, &u16));
    TEST_ASSERT_EQUAL(4321, u16);
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u32(&F_U32, &u32));
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, u32);
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_i32(&F_I32, &i32));
    TEST_ASSERT_EQUAL(-12345, i32);
}

/* ---------------------------------------------------------------------------
 * 1c. blob round trip (single field session).
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_blob_round_trip(void)
{
    reset_all();

    uint8_t payload[] = {1, 2, 3, 4};
    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_blob(&h, &F_BLOB, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_commit(&h));

    uint8_t out[16] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_blob(&F_BLOB, out, sizeof(out), &out_len));
    TEST_ASSERT_EQUAL(sizeof(payload), out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, out, sizeof(payload));
}

/* ---------------------------------------------------------------------------
 * 2. discard -> nothing lands.
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_discard_lands_nothing(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_u8(&h, &F_U8, 9));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_str(&h, &F_STR, "bye"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_discard(&h));

    TEST_ASSERT_FALSE(bb_config_exists(&F_U8));
    TEST_ASSERT_FALSE(bb_config_exists(&F_STR));
}

/* ---------------------------------------------------------------------------
 * 3. Delegated (txn-level) oversize value: the offending set_* itself
 *    returns the txn's NO_SPACE (not a local sticky error); commit() then
 *    delegates straight to bb_storage_txn_commit(), which returns the same
 *    sticky NO_SPACE -- and lands NOTHING, including the earlier valid stage.
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_oversize_value_poisons_txn_commit_returns_sticky_lands_nothing(void)
{
    reset_all();

    uint8_t big[100];
    memset(big, 'x', sizeof(big));

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_u8(&h, &F_U8, 1));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_config_staged_set_blob(&h, &F_BLOB_BIG, big, sizeof(big)));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_config_staged_commit(&h));

    TEST_ASSERT_FALSE(bb_config_exists(&F_U8));
    TEST_ASSERT_FALSE(bb_config_exists(&F_BLOB_BIG));
}

/* ---------------------------------------------------------------------------
 * 4. LOCAL precheck poisons independently of the txn: stage a valid field,
 *    then call set_* with a WRONG type on a second field -> INVALID_ARG
 *    without ever reaching the txn -> commit() STILL returns that sticky
 *    error (aborting the txn itself) and nothing lands.
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_local_precheck_poisons_independently_of_txn(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_u8(&h, &F_U8, 5));
    // F_STR is BB_CONFIG_STR; calling set_u16 against it is a type mismatch.
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_u16(&h, &F_STR, 1));
    // A subsequent, otherwise-valid call short-circuits on the local sticky
    // error without touching the txn.
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_bool(&h, &F_BOOL, true));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_commit(&h));

    TEST_ASSERT_FALSE(bb_config_exists(&F_U8));
    TEST_ASSERT_FALSE(bb_config_exists(&F_BOOL));
}

/* ---------------------------------------------------------------------------
 * 5. Type-mismatch precheck (each setter validates f->type independently).
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_type_mismatch_returns_invalid_arg(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_bool(&h, &F_U8, true));
}

/* ---------------------------------------------------------------------------
 * 5b. Cross-namespace / cross-backend field precheck.
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_cross_namespace_field_returns_invalid_arg(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_u8(&h, &F_CROSS_NS, 1));
}

void test_bb_config_staged_cross_backend_field_returns_invalid_arg(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_u8(&h, &F_CROSS_BACKEND, 1));
}

/* ---------------------------------------------------------------------------
 * 6. str/blob over max_len precheck (mirrors bb_config_set_str/_blob).
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_str_over_max_len_returns_invalid_arg(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    // F_STR.max_len == 8; "toolongstring" (13 chars) doesn't fit.
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_str(&h, &F_STR, "toolongstring"));
}

void test_bb_config_staged_blob_over_max_len_returns_invalid_arg(void)
{
    reset_all();

    uint8_t big[9]; // F_BLOB.max_len == 8
    memset(big, 0, sizeof(big));

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_blob(&h, &F_BLOB, big, sizeof(big)));
}

/* ---------------------------------------------------------------------------
 * 7. commit-after-close (double commit) -> BB_ERR_INVALID_STATE, delegated
 *    from bb_storage_txn_commit on a closed txn.
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_double_commit_returns_invalid_state(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_u8(&h, &F_U8, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_commit(&h));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_config_staged_commit(&h));
}

/* ---------------------------------------------------------------------------
 * 8. discard idempotent: never-begun (zero-init), already-committed, and
 *    already-discarded handles are all safe to discard again.
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_discard_idempotent_never_begun(void)
{
    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_discard(&h));
}

void test_bb_config_staged_discard_idempotent_after_commit(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_u8(&h, &F_U8, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_commit(&h));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_discard(&h));
}

void test_bb_config_staged_discard_idempotent_after_discard(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_u8(&h, &F_U8, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_discard(&h));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_discard(&h));
}

/* ---------------------------------------------------------------------------
 * 9. NULL-handle guard on every verb.
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_null_handle_returns_invalid_arg_for_every_verb(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_begin(NULL, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_commit(NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_discard(NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_bool(NULL, &F_BOOL, true));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_u8(NULL, &F_U8, 1));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_u16(NULL, &F_U16, 1));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_u32(NULL, &F_U32, 1));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_i32(NULL, &F_I32, 1));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_str(NULL, &F_STR, "x"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_blob(NULL, &F_BLOB, "x", 1));
}

/* ---------------------------------------------------------------------------
 * 11. NULL field guard on precheck() -- both call sites: the scalar path
 *     (via stage_scalar()) and the str/blob path (precheck() called
 *     directly). h is non-NULL and begun; f is NULL.
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_null_field_scalar_returns_invalid_arg(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_u8(&h, NULL, 1));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_commit(&h));

    TEST_ASSERT_FALSE(bb_config_exists(&F_U8));
}

void test_bb_config_staged_null_field_str_returns_invalid_arg(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_str(&h, NULL, "hi"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_commit(&h));

    TEST_ASSERT_FALSE(bb_config_exists(&F_STR));
}

/* ---------------------------------------------------------------------------
 * 12. Symmetric closed-session contract: double commit after the POISONED
 *     (local-precheck) path returns the sticky local error on the FIRST
 *     call, then BB_ERR_INVALID_STATE on the SECOND -- never the sticky
 *     error twice.
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_double_commit_after_poisoned_commit_returns_invalid_state_on_second(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    // F_STR is BB_CONFIG_STR; calling set_u16 against it is a type mismatch,
    // poisoning h->_local_err.
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_u16(&h, &F_STR, 1));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_commit(&h));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_config_staged_commit(&h));
}

/* ---------------------------------------------------------------------------
 * 13. set_* after commit (clean-path close) -> BB_ERR_INVALID_STATE, session
 *     accepts no more stages.
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_set_after_commit_returns_invalid_state(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_u8(&h, &F_U8, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_commit(&h));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_config_staged_set_u8(&h, &F_U8, 2));
}

// NOTE: discard-after-commit and discard-after-discard idempotency (still
// BB_OK with the new _closed guard) are already covered by
// test_bb_config_staged_discard_idempotent_after_commit and
// test_bb_config_staged_discard_idempotent_after_discard above (section 8).

/* ---------------------------------------------------------------------------
 * 14. begin() resets a REUSED handle: a struct previously closed via a full
 *     commit cycle (dirty _closed=true, _local_err==BB_OK) behaves as a
 *     fresh session on a second begin() -- set_* succeeds, commit()
 *     delegates normally, and the staged value reads back. (On a zero-init
 *     handle the reset assignments are no-ops, so this exercises the
 *     non-trivial path.)
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_begin_resets_reused_handle_after_commit(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_u8(&h, &F_U8, 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_commit(&h));

    // h is now closed (_closed == true). Reuse the SAME struct without a
    // fresh zero-init -- begin() must reset _closed/_local_err itself.
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_u16(&h, &F_U16, 4321));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_commit(&h));

    uint16_t u16 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u16(&F_U16, &u16));
    TEST_ASSERT_EQUAL(4321, u16);
}

/* ---------------------------------------------------------------------------
 * 15. begin() resets a REUSED handle that was closed via a POISONED commit
 *     (dirty _local_err != BB_OK, not just _closed). Reused, the sticky
 *     local error must not leak into the new session.
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_begin_resets_reused_handle_after_poisoned_commit(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    // F_STR is BB_CONFIG_STR; calling set_u16 against it is a type mismatch,
    // poisoning h->_local_err.
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_u16(&h, &F_STR, 1));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_commit(&h));

    // Reuse the SAME dirty struct (_closed=true, _local_err=INVALID_ARG).
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_set_u8(&h, &F_U8, 42));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_commit(&h));

    uint8_t u8 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_config_get_u8(&F_U8, &u8));
    TEST_ASSERT_EQUAL(42, u8);
}

/* ---------------------------------------------------------------------------
 * 10. NULL value guards on str/blob setters (mirrors bb_config_set_str/_blob).
 * ---------------------------------------------------------------------------*/
void test_bb_config_staged_null_value_returns_invalid_arg(void)
{
    reset_all();

    bb_config_staged_t h = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_str(&h, &F_STR, NULL));

    bb_config_staged_t h2 = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_config_staged_begin(&h2, "ram", "ns"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_config_staged_set_blob(&h2, &F_BLOB, NULL, 4));
}

#include "unity.h"
#include "bb_storage.h"
#include "bb_storage_ram.h"
#include "bb_core.h"

#include <string.h>

// Facade contract for bb_storage_txn_begin/set/commit/abort: dispatch to the
// backend's captured txn hooks, sticky-poison on first error, the optional
// four-member txn group validated at registration (all-NULL or all-set —
// no partial group, no sequential fallback). Backend-specific atomicity is
// covered by test_bb_storage_ram_txn.c against the real ram backend.

static void reset_all(void)
{
    bb_storage_test_reset();
    bb_storage_ram_test_reset();
}

/* ---------------------------------------------------------------------------
 * Fake backend with a full txn group — proves the facade dispatches to it
 * and preserves call order / sticky-err semantics.
 * ---------------------------------------------------------------------------*/
static bb_err_t fake_get(void *impl, const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len)
{
    (void)impl; (void)addr; (void)buf; (void)cap;
    *out_len = 0;
    return BB_OK;
}
static bb_err_t fake_set(void *impl, const bb_storage_addr_t *addr, const void *buf, size_t len)
{
    (void)impl; (void)addr; (void)buf; (void)len;
    return BB_OK;
}
static bb_err_t fake_erase(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl; (void)addr;
    return BB_OK;
}
static bool fake_exists(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl; (void)addr;
    return false;
}

#define FAKE_TXN_LOG_MAX 8
static char     s_txn_log_keys[FAKE_TXN_LOG_MAX][BB_STORAGE_TXN_KEY_MAX_BYTES];
static int      s_txn_log_count;
static int      s_txn_begin_calls;
static int      s_txn_commit_calls;
static int      s_txn_abort_calls;
static bool     s_txn_fail_second_set;

static void fake_txn_reset(void)
{
    memset(s_txn_log_keys, 0, sizeof(s_txn_log_keys));
    s_txn_log_count     = 0;
    s_txn_begin_calls   = 0;
    s_txn_commit_calls  = 0;
    s_txn_abort_calls   = 0;
    s_txn_fail_second_set = false;
}

static bb_err_t fake_txn_begin(void *impl, bb_storage_txn_t *txn, const char *ns_or_dir)
{
    (void)impl; (void)ns_or_dir;
    s_txn_begin_calls++;
    txn->_open = 1;
    txn->_err  = BB_OK;
    return BB_OK;
}
static bb_err_t fake_txn_set(void *impl, bb_storage_txn_t *txn, const char *key, bb_storage_enc_t enc,
                              const void *buf, size_t len)
{
    (void)impl; (void)txn; (void)enc; (void)buf; (void)len;
    if (s_txn_fail_second_set && s_txn_log_count == 1) {
        return BB_ERR_NO_SPACE;
    }
    if (s_txn_log_count < FAKE_TXN_LOG_MAX) {
        strncpy(s_txn_log_keys[s_txn_log_count], key, BB_STORAGE_TXN_KEY_MAX_BYTES - 1);
        s_txn_log_count++;
    }
    return BB_OK;
}
static bb_err_t fake_txn_commit(void *impl, bb_storage_txn_t *txn)
{
    (void)impl;
    s_txn_commit_calls++;
    bb_err_t err = txn->_err;
    txn->_open = 0;
    return err;
}
static bb_err_t fake_txn_abort(void *impl, bb_storage_txn_t *txn)
{
    (void)impl;
    s_txn_abort_calls++;
    txn->_open = 0;
    return BB_OK;
}

static void register_fake_txn_backend(void)
{
    fake_txn_reset();
    bb_storage_vtable_t vt = {
        .get        = fake_get,
        .set        = fake_set,
        .erase      = fake_erase,
        .exists     = fake_exists,
        .txn_begin  = fake_txn_begin,
        .txn_set    = fake_txn_set,
        .txn_commit = fake_txn_commit,
        .txn_abort  = fake_txn_abort,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_register_backend("fake_txn", &vt, NULL));
}

static void register_no_txn_backend(void)
{
    bb_storage_vtable_t vt = { .get = fake_get, .set = fake_set, .erase = fake_erase, .exists = fake_exists };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_register_backend("fake_no_txn", &vt, NULL));
}

/* ---------------------------------------------------------------------------
 * 1. txn_begin vs all-NULL-txn-group backend -> BB_ERR_UNSUPPORTED
 * ---------------------------------------------------------------------------*/
void test_bb_storage_txn_begin_unsupported_backend_returns_unsupported(void)
{
    reset_all();
    register_no_txn_backend();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_storage_txn_begin("fake_no_txn", "ns", &txn));
}

/* ---------------------------------------------------------------------------
 * 2. txn_begin vs unknown backend -> BB_ERR_NOT_FOUND
 * ---------------------------------------------------------------------------*/
void test_bb_storage_txn_begin_unknown_backend_returns_not_found(void)
{
    reset_all();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_txn_begin("does_not_exist", "ns", &txn));
}

/* ---------------------------------------------------------------------------
 * 3. register_backend rejects a vtable with exactly one txn member set
 * ---------------------------------------------------------------------------*/
void test_bb_storage_register_backend_partial_txn_group_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_vtable_t vt = {
        .get = fake_get, .set = fake_set, .erase = fake_erase, .exists = fake_exists,
        .txn_begin = fake_txn_begin, /* txn_set/commit/abort left NULL */
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_register_backend("partial_txn", &vt, NULL));
}

/* ---------------------------------------------------------------------------
 * 4. begin -> set(k1) -> set(k2) -> commit calls commit once, both keys
 *    staged in order, sticky-err correct (none).
 * ---------------------------------------------------------------------------*/
void test_bb_storage_txn_commit_stages_both_keys_in_order(void)
{
    reset_all();
    register_fake_txn_backend();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("fake_txn", "ns", &txn));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "k1", BB_STORAGE_ENC_STR, "a", 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "k2", BB_STORAGE_ENC_STR, "b", 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_commit(&txn));

    TEST_ASSERT_EQUAL(1, s_txn_begin_calls);
    TEST_ASSERT_EQUAL(1, s_txn_commit_calls);
    TEST_ASSERT_EQUAL(2, s_txn_log_count);
    TEST_ASSERT_EQUAL_STRING("k1", s_txn_log_keys[0]);
    TEST_ASSERT_EQUAL_STRING("k2", s_txn_log_keys[1]);
}

/* ---------------------------------------------------------------------------
 * 5. begin -> set(k1) -> set(fails) -> commit returns the sticky error.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_txn_commit_returns_sticky_error_from_failed_set(void)
{
    reset_all();
    register_fake_txn_backend();
    s_txn_fail_second_set = true;

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("fake_txn", "ns", &txn));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "k1", BB_STORAGE_ENC_STR, "a", 1));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_storage_txn_set(&txn, "k2", BB_STORAGE_ENC_STR, "b", 1));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_storage_txn_commit(&txn));
}

/* ---------------------------------------------------------------------------
 * 6. set/commit/abort on never-begun or already-closed txn.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_txn_set_on_never_begun_txn_returns_invalid_state(void)
{
    reset_all();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_storage_txn_set(&txn, "k", BB_STORAGE_ENC_STR, "a", 1));
}

void test_bb_storage_txn_set_on_already_closed_txn_returns_invalid_state(void)
{
    reset_all();
    register_fake_txn_backend();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("fake_txn", "ns", &txn));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_commit(&txn));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_storage_txn_set(&txn, "k", BB_STORAGE_ENC_STR, "a", 1));
}

void test_bb_storage_txn_abort_on_never_begun_txn_is_safe(void)
{
    reset_all();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_abort(&txn));
}

/* ---------------------------------------------------------------------------
 * 6b. commit is NOT idempotent like abort — a second commit (or a commit on
 *     a never-begun txn) must surface BB_ERR_INVALID_STATE, not a silent
 *     BB_OK, so callers can't mistake a no-op for a successful re-commit.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_txn_double_commit_returns_invalid_state(void)
{
    reset_all();
    register_fake_txn_backend();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("fake_txn", "ns", &txn));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_set(&txn, "k1", BB_STORAGE_ENC_STR, "a", 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_commit(&txn));
    TEST_ASSERT_EQUAL(1, s_txn_commit_calls);

    /* second commit: txn is closed, must not silently succeed or re-dispatch */
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_storage_txn_commit(&txn));
    TEST_ASSERT_EQUAL(1, s_txn_commit_calls);
}

void test_bb_storage_txn_commit_never_begun_returns_invalid_state(void)
{
    reset_all();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_storage_txn_commit(&txn));
}

void test_bb_storage_txn_abort_on_already_closed_txn_is_idempotent(void)
{
    reset_all();
    register_fake_txn_backend();

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("fake_txn", "ns", &txn));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_abort(&txn));
    TEST_ASSERT_EQUAL(1, s_txn_abort_calls);
    /* second abort: already closed, no backend dispatch, still BB_OK */
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_abort(&txn));
    TEST_ASSERT_EQUAL(1, s_txn_abort_calls);
}

/* ---------------------------------------------------------------------------
 * Facade-level NULL/arg validation.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_txn_begin_null_args_return_invalid_arg(void)
{
    reset_all();
    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_txn_begin(NULL, "ns", &txn));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_txn_begin("ram", NULL, &txn));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_txn_begin("ram", "ns", NULL));
}

void test_bb_storage_txn_set_null_args_return_invalid_arg(void)
{
    reset_all();
    register_fake_txn_backend();
    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_txn_begin("fake_txn", "ns", &txn));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_txn_set(NULL, "k", BB_STORAGE_ENC_STR, "a", 1));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_txn_set(&txn, NULL, BB_STORAGE_ENC_STR, "a", 1));
}

void test_bb_storage_txn_commit_null_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_txn_commit(NULL));
}

void test_bb_storage_txn_abort_null_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_txn_abort(NULL));
}

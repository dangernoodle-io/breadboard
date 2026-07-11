#include "unity.h"
#include "bb_storage.h"
#include "bb_storage_nvs.h"
#include "bb_core.h"

#include <string.h>

// bb_storage_nvs's txn-primitive seam (bb_storage_nvs_txn_ops_t,
// BB_STORAGE_NVS_TESTING-gated): drives nvs_txn_begin/set/commit/abort's
// open->set*->commit->close orchestration against a fake, without any real
// NVS/ESP-IDF backing. Covers the atomicity/handle-lifecycle contract that
// only ever runs on esp32 otherwise (bb_storage_nvs_register() itself
// returns BB_ERR_UNSUPPORTED on host; this seam bypasses that facade layer
// entirely and drives the orchestration directly).

#define FAKE_LOG_MAX 8
#define FAKE_KEY_BUF 32

static char     s_set_log_keys[FAKE_LOG_MAX][FAKE_KEY_BUF];
static int      s_set_log_count;      // successfully-applied set() calls
static int      s_set_dispatch_calls; // every fake set_* invocation, success or fail
static int      s_open_calls;
static int      s_commit_calls;
static int      s_close_calls;
static bb_err_t s_open_err;
static int      s_fail_on_set_index;  // -1 = never fail; else 0-based dispatch index to fail
static uint32_t s_next_handle;

#define TRACE_MAX 16
static const char *s_trace[TRACE_MAX];
static int          s_trace_count;

static void trace(const char *op)
{
    if (s_trace_count < TRACE_MAX) {
        s_trace[s_trace_count++] = op;
    }
}

static void fake_reset(void)
{
    memset(s_set_log_keys, 0, sizeof(s_set_log_keys));
    s_set_log_count      = 0;
    s_set_dispatch_calls = 0;
    s_open_calls         = 0;
    s_commit_calls       = 0;
    s_close_calls        = 0;
    s_open_err           = BB_OK;
    s_fail_on_set_index  = -1;
    s_next_handle        = 1;
    s_trace_count        = 0;
    memset(s_trace, 0, sizeof(s_trace));
}

static bb_err_t fake_open(const char *ns, uint32_t *out_handle)
{
    (void)ns;
    s_open_calls++;
    trace("open");
    if (s_open_err != BB_OK) {
        return s_open_err;
    }
    *out_handle = s_next_handle++;
    return BB_OK;
}

static bb_err_t fake_set_generic(const char *key)
{
    int idx = s_set_dispatch_calls;
    s_set_dispatch_calls++;
    trace("set");

    if (s_fail_on_set_index >= 0 && idx == s_fail_on_set_index) {
        return BB_ERR_INVALID_ARG;
    }
    if (s_set_log_count < FAKE_LOG_MAX) {
        strncpy(s_set_log_keys[s_set_log_count], key, FAKE_KEY_BUF - 1);
    }
    s_set_log_count++;
    return BB_OK;
}

static bb_err_t fake_set_u8(uint32_t handle, const char *key, uint8_t value)
{
    (void)handle; (void)value;
    return fake_set_generic(key);
}
static bb_err_t fake_set_u16(uint32_t handle, const char *key, uint16_t value)
{
    (void)handle; (void)value;
    return fake_set_generic(key);
}
static bb_err_t fake_set_u32(uint32_t handle, const char *key, uint32_t value)
{
    (void)handle; (void)value;
    return fake_set_generic(key);
}
static bb_err_t fake_set_i32(uint32_t handle, const char *key, int32_t value)
{
    (void)handle; (void)value;
    return fake_set_generic(key);
}
static bb_err_t fake_set_str(uint32_t handle, const char *key, const char *value)
{
    (void)handle; (void)value;
    return fake_set_generic(key);
}
static bb_err_t fake_set_blob(uint32_t handle, const char *key, const void *buf, size_t len)
{
    (void)handle; (void)buf; (void)len;
    return fake_set_generic(key);
}

static bb_err_t fake_commit(uint32_t handle)
{
    (void)handle;
    s_commit_calls++;
    trace("commit");
    return BB_OK;
}

static void fake_close(uint32_t handle)
{
    (void)handle;
    s_close_calls++;
    trace("close");
}

static const bb_storage_nvs_txn_ops_t s_fake_ops = {
    .open     = fake_open,
    .set_u8   = fake_set_u8,
    .set_u16  = fake_set_u16,
    .set_u32  = fake_set_u32,
    .set_i32  = fake_set_i32,
    .set_str  = fake_set_str,
    .set_blob = fake_set_blob,
    .commit   = fake_commit,
    .close    = fake_close,
};

/* ---------------------------------------------------------------------------
 * 1. commit calls set(s), then commit, then close — in that order.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_nvs_txn_commit_calls_set_then_commit_then_close_in_order(void)
{
    fake_reset();
    bb_storage_nvs_set_txn_ops_for_test(&s_fake_ops);

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_begin_for_test(&txn, "wifi"));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_set_for_test(&txn, "ssid", BB_STORAGE_ENC_STR, "ap", 2));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_set_for_test(&txn, "pass", BB_STORAGE_ENC_STR, "secret", 6));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_commit_for_test(&txn));

    TEST_ASSERT_EQUAL(1, s_open_calls);
    TEST_ASSERT_EQUAL(2, s_set_log_count);
    TEST_ASSERT_EQUAL(1, s_commit_calls);
    TEST_ASSERT_EQUAL(1, s_close_calls);

    TEST_ASSERT_EQUAL(5, s_trace_count);
    TEST_ASSERT_EQUAL_STRING("open", s_trace[0]);
    TEST_ASSERT_EQUAL_STRING("set", s_trace[1]);
    TEST_ASSERT_EQUAL_STRING("set", s_trace[2]);
    TEST_ASSERT_EQUAL_STRING("commit", s_trace[3]);
    TEST_ASSERT_EQUAL_STRING("close", s_trace[4]);

    TEST_ASSERT_EQUAL_STRING("ssid", s_set_log_keys[0]);
    TEST_ASSERT_EQUAL_STRING("pass", s_set_log_keys[1]);
    bb_storage_nvs_set_txn_ops_for_test(NULL);
}

/* ---------------------------------------------------------------------------
 * 2. abort closes the handle WITHOUT ever calling commit.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_nvs_txn_abort_closes_without_commit(void)
{
    fake_reset();
    bb_storage_nvs_set_txn_ops_for_test(&s_fake_ops);

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_begin_for_test(&txn, "wifi"));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_set_for_test(&txn, "ssid", BB_STORAGE_ENC_STR, "ap", 2));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_abort_for_test(&txn));

    TEST_ASSERT_EQUAL(0, s_commit_calls);
    TEST_ASSERT_EQUAL(1, s_close_calls);
    TEST_ASSERT_EQUAL(3, s_trace_count);
    TEST_ASSERT_EQUAL_STRING("open", s_trace[0]);
    TEST_ASSERT_EQUAL_STRING("set", s_trace[1]);
    TEST_ASSERT_EQUAL_STRING("close", s_trace[2]);
    bb_storage_nvs_set_txn_ops_for_test(NULL);
}

/* ---------------------------------------------------------------------------
 * 3. open() failing must not fabricate a handle -- the txn never becomes
 *    "open", so a subsequent abort() is a safe no-op that does NOT call
 *    close() on a handle that was never acquired.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_nvs_txn_open_error_leaves_txn_closed(void)
{
    fake_reset();
    s_open_err = BB_ERR_NO_SPACE;
    bb_storage_nvs_set_txn_ops_for_test(&s_fake_ops);

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_storage_nvs_txn_begin_for_test(&txn, "wifi"));
    TEST_ASSERT_EQUAL(1, s_open_calls);
    TEST_ASSERT_EQUAL(0, s_close_calls);

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_abort_for_test(&txn));
    TEST_ASSERT_EQUAL(0, s_close_calls);
    bb_storage_nvs_set_txn_ops_for_test(NULL);
}

/* ---------------------------------------------------------------------------
 * 4. A failing set() poisons the txn (sticky error); commit() must NOT call
 *    the backend's commit() (nothing to durably commit), but MUST still
 *    close the handle -- no leak on the error path.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_nvs_txn_set_error_closes_handle_on_commit(void)
{
    fake_reset();
    s_fail_on_set_index = 0;
    bb_storage_nvs_set_txn_ops_for_test(&s_fake_ops);

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_begin_for_test(&txn, "wifi"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_storage_nvs_txn_set_for_test(&txn, "ssid", BB_STORAGE_ENC_STR, "ap", 2));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_nvs_txn_commit_for_test(&txn));

    TEST_ASSERT_EQUAL(0, s_commit_calls);
    TEST_ASSERT_EQUAL(1, s_close_calls);
    bb_storage_nvs_set_txn_ops_for_test(NULL);
}

/* ---------------------------------------------------------------------------
 * 5. Sticky error short-circuits every subsequent set() (never reaches the
 *    backend dispatch again) and commit() returns the sticky error while
 *    still closing.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_nvs_txn_sticky_error_short_circuits_set_and_commit(void)
{
    fake_reset();
    s_fail_on_set_index = 0;
    bb_storage_nvs_set_txn_ops_for_test(&s_fake_ops);

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_begin_for_test(&txn, "wifi"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_storage_nvs_txn_set_for_test(&txn, "k1", BB_STORAGE_ENC_STR, "a", 1));
    TEST_ASSERT_EQUAL(1, s_set_dispatch_calls);

    // second set: sticky-poisoned txn must short-circuit BEFORE dispatching
    // to the backend at all.
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_storage_nvs_txn_set_for_test(&txn, "k2", BB_STORAGE_ENC_STR, "b", 1));
    TEST_ASSERT_EQUAL(1, s_set_dispatch_calls);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_nvs_txn_commit_for_test(&txn));
    TEST_ASSERT_EQUAL(0, s_commit_calls);
    TEST_ASSERT_EQUAL(1, s_close_calls);
    bb_storage_nvs_set_txn_ops_for_test(NULL);
}

/* ---------------------------------------------------------------------------
 * 6-12. nvs_txn_set's encoding-classify switch, per BB_STORAGE_NVS_KIND_*:
 * every reject path must return the header-documented error code AND must
 * NOT dispatch to the backend's set_* — a rejected set() leaves nothing
 * written, preserving the all-or-nothing atomicity guarantee.
 * ---------------------------------------------------------------------------*/

// Oversize value: STR at len > BB_STORAGE_TXN_VALUE_MAX_BYTES -> BB_ERR_NO_SPACE.
void test_bb_storage_nvs_txn_set_str_oversize_returns_no_space_and_does_not_dispatch(void)
{
    fake_reset();
    bb_storage_nvs_set_txn_ops_for_test(&s_fake_ops);

    static uint8_t oversize[BB_STORAGE_TXN_VALUE_MAX_BYTES + 1];

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_begin_for_test(&txn, "wifi"));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                       bb_storage_nvs_txn_set_for_test(&txn, "ssid", BB_STORAGE_ENC_STR,
                                                        oversize, sizeof(oversize)));

    TEST_ASSERT_EQUAL(0, s_set_dispatch_calls);
    TEST_ASSERT_EQUAL(0, s_set_log_count);
    bb_storage_nvs_set_txn_ops_for_test(NULL);
}

// Oversize value: BLOB (the default encoding) at len > BB_STORAGE_TXN_VALUE_MAX_BYTES
// -> BB_ERR_NO_SPACE, same as STR — the cap is uniform across every encoding.
void test_bb_storage_nvs_txn_set_blob_oversize_returns_no_space_and_does_not_dispatch(void)
{
    fake_reset();
    bb_storage_nvs_set_txn_ops_for_test(&s_fake_ops);

    static uint8_t oversize[BB_STORAGE_TXN_VALUE_MAX_BYTES + 1];

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_begin_for_test(&txn, "wifi"));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                       bb_storage_nvs_txn_set_for_test(&txn, "blob", BB_STORAGE_ENC_BLOB,
                                                        oversize, sizeof(oversize)));

    TEST_ASSERT_EQUAL(0, s_set_dispatch_calls);
    TEST_ASSERT_EQUAL(0, s_set_log_count);
    bb_storage_nvs_set_txn_ops_for_test(NULL);
}

// Wrong-length U8: len != sizeof(uint8_t) -> BB_ERR_INVALID_ARG, no dispatch.
void test_bb_storage_nvs_txn_set_u8_wrong_length_returns_invalid_arg_and_does_not_dispatch(void)
{
    fake_reset();
    bb_storage_nvs_set_txn_ops_for_test(&s_fake_ops);

    uint8_t buf[2] = {0};

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_begin_for_test(&txn, "wifi"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_storage_nvs_txn_set_for_test(&txn, "u8", BB_STORAGE_ENC_U8, buf, 2));

    TEST_ASSERT_EQUAL(0, s_set_dispatch_calls);
    TEST_ASSERT_EQUAL(0, s_set_log_count);
    bb_storage_nvs_set_txn_ops_for_test(NULL);
}

// Wrong-length U16: len != sizeof(uint16_t) -> BB_ERR_INVALID_ARG, no dispatch.
void test_bb_storage_nvs_txn_set_u16_wrong_length_returns_invalid_arg_and_does_not_dispatch(void)
{
    fake_reset();
    bb_storage_nvs_set_txn_ops_for_test(&s_fake_ops);

    uint8_t buf[1] = {0};

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_begin_for_test(&txn, "wifi"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_storage_nvs_txn_set_for_test(&txn, "u16", BB_STORAGE_ENC_U16, buf, 1));

    TEST_ASSERT_EQUAL(0, s_set_dispatch_calls);
    TEST_ASSERT_EQUAL(0, s_set_log_count);
    bb_storage_nvs_set_txn_ops_for_test(NULL);
}

// Wrong-length U32: len != sizeof(uint32_t) -> BB_ERR_INVALID_ARG, no dispatch.
void test_bb_storage_nvs_txn_set_u32_wrong_length_returns_invalid_arg_and_does_not_dispatch(void)
{
    fake_reset();
    bb_storage_nvs_set_txn_ops_for_test(&s_fake_ops);

    uint8_t buf[2] = {0};

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_begin_for_test(&txn, "wifi"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_storage_nvs_txn_set_for_test(&txn, "u32", BB_STORAGE_ENC_U32, buf, 2));

    TEST_ASSERT_EQUAL(0, s_set_dispatch_calls);
    TEST_ASSERT_EQUAL(0, s_set_log_count);
    bb_storage_nvs_set_txn_ops_for_test(NULL);
}

// Wrong-length I32: len != sizeof(uint32_t) -> BB_ERR_INVALID_ARG, no dispatch.
void test_bb_storage_nvs_txn_set_i32_wrong_length_returns_invalid_arg_and_does_not_dispatch(void)
{
    fake_reset();
    bb_storage_nvs_set_txn_ops_for_test(&s_fake_ops);

    uint8_t buf[2] = {0};

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_begin_for_test(&txn, "wifi"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_storage_nvs_txn_set_for_test(&txn, "i32", BB_STORAGE_ENC_I32, buf, 2));

    TEST_ASSERT_EQUAL(0, s_set_dispatch_calls);
    TEST_ASSERT_EQUAL(0, s_set_log_count);
    bb_storage_nvs_set_txn_ops_for_test(NULL);
}

// Key at/over the NVS backend's real key-name limit (15 chars + NUL) ->
// BB_ERR_INVALID_ARG, checked BEFORE the value-size cap and BEFORE any
// dispatch -- a 16-char key is rejected even with an otherwise-legal value.
void test_bb_storage_nvs_txn_set_key_over_nvs_limit_returns_invalid_arg_and_does_not_dispatch(void)
{
    fake_reset();
    bb_storage_nvs_set_txn_ops_for_test(&s_fake_ops);

    const char *long_key = "sixteen_chars!!!";  // 16 chars, at the NVS limit
    TEST_ASSERT_EQUAL(16u, strlen(long_key));

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_begin_for_test(&txn, "wifi"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_storage_nvs_txn_set_for_test(&txn, long_key, BB_STORAGE_ENC_STR, "ap", 2));

    TEST_ASSERT_EQUAL(0, s_set_dispatch_calls);
    TEST_ASSERT_EQUAL(0, s_set_log_count);
    bb_storage_nvs_set_txn_ops_for_test(NULL);
}

// Key at the NVS backend's real usable limit (15 chars, fits with the NUL in
// the 16-byte NVS key slot) -> ACCEPTED, dispatches to the backend set_*.
// Positive-boundary counterpart to the 16-char-over-limit reject test above.
void test_bb_storage_nvs_txn_set_key_at_nvs_limit_returns_ok_and_dispatches(void)
{
    fake_reset();
    bb_storage_nvs_set_txn_ops_for_test(&s_fake_ops);

    const char *max_key = "fifteen_chars!!";  // 15 chars, at the usable limit
    TEST_ASSERT_EQUAL(15u, strlen(max_key));

    bb_storage_txn_t txn = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_txn_begin_for_test(&txn, "wifi"));
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_storage_nvs_txn_set_for_test(&txn, max_key, BB_STORAGE_ENC_STR, "ap", 2));

    TEST_ASSERT_EQUAL(1, s_set_dispatch_calls);
    TEST_ASSERT_EQUAL(1, s_set_log_count);
    bb_storage_nvs_set_txn_ops_for_test(NULL);
}

#include "unity.h"
#include "bb_dispatch_cmd.h"
#include "bb_core.h"
#include "bb_json.h"

#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Stub handlers
 * ---------------------------------------------------------------------------*/
static bb_err_t handler_ok(bb_json_t args, bb_json_t result_out, void *ctx)
{
    (void)args;
    (void)ctx;
    bb_json_obj_set_bool(result_out, "handled", true);
    return BB_OK;
}

static bb_err_t handler_fail(bb_json_t args, bb_json_t result_out, void *ctx)
{
    (void)args;
    (void)result_out;
    (void)ctx;
    return BB_ERR_INVALID_STATE;
}

static int s_handler_calls;
static bb_err_t handler_counting(bb_json_t args, bb_json_t result_out, void *ctx)
{
    (void)args;
    (void)result_out;
    (void)ctx;
    s_handler_calls++;
    return BB_OK;
}

/* ---------------------------------------------------------------------------
 * Stub authorizers
 * ---------------------------------------------------------------------------*/
static bool authorizer_allow(const char *action, bb_json_t args, void *ctx)
{
    (void)action;
    (void)args;
    (void)ctx;
    return true;
}

static bool authorizer_reject(const char *action, bb_json_t args, void *ctx)
{
    (void)action;
    (void)args;
    (void)ctx;
    return false;
}

/* setUp calls bb_dispatch_cmd_test_reset() so each test starts clean. */

/* ---------------------------------------------------------------------------
 * register: ok
 * ---------------------------------------------------------------------------*/
void test_bb_dispatch_cmd_register_ok(void)
{
    bb_dispatch_cmd_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_dispatch_cmd_register("get_status", handler_ok, NULL));
    TEST_ASSERT_EQUAL(1, (int)bb_dispatch_cmd_count());
}

/* ---------------------------------------------------------------------------
 * register: duplicate action rejected, first registration wins
 * ---------------------------------------------------------------------------*/
void test_bb_dispatch_cmd_register_dup_rejected(void)
{
    bb_dispatch_cmd_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_dispatch_cmd_register("get_status", handler_ok, NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
        bb_dispatch_cmd_register("get_status", handler_fail, NULL));
    TEST_ASSERT_EQUAL(1, (int)bb_dispatch_cmd_count());

    /* First registration still dispatches. */
    bb_json_t result = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_OK, bb_dispatch_cmd_call("get_status", NULL, result));
    bb_json_free(result);
}

/* ---------------------------------------------------------------------------
 * register: null action / handler → BB_ERR_INVALID_ARG
 * ---------------------------------------------------------------------------*/
void test_bb_dispatch_cmd_register_null_action_returns_invalid_arg(void)
{
    bb_dispatch_cmd_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_dispatch_cmd_register(NULL, handler_ok, NULL));
    TEST_ASSERT_EQUAL(0, (int)bb_dispatch_cmd_count());
}

void test_bb_dispatch_cmd_register_null_handler_returns_invalid_arg(void)
{
    bb_dispatch_cmd_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_dispatch_cmd_register("get_status", NULL, NULL));
    TEST_ASSERT_EQUAL(0, (int)bb_dispatch_cmd_count());
}

/* ---------------------------------------------------------------------------
 * register: overflow returns BB_ERR_NO_SPACE, count never exceeds cap
 * ---------------------------------------------------------------------------*/
void test_bb_dispatch_cmd_register_overflow_returns_no_space(void)
{
    bb_dispatch_cmd_test_reset();

    static char actions[BB_DISPATCH_CMD_CAP][24];
    for (int i = 0; i < BB_DISPATCH_CMD_CAP; i++) {
        snprintf(actions[i], sizeof(actions[i]), "action_%d", i);
        TEST_ASSERT_EQUAL(BB_OK, bb_dispatch_cmd_register(actions[i], handler_ok, NULL));
    }
    TEST_ASSERT_EQUAL(BB_DISPATCH_CMD_CAP, (int)bb_dispatch_cmd_count());

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
        bb_dispatch_cmd_register("one_too_many", handler_ok, NULL));
    TEST_ASSERT_EQUAL(BB_DISPATCH_CMD_CAP, (int)bb_dispatch_cmd_count());
}

/* ---------------------------------------------------------------------------
 * call: ok — handler runs, result filled
 * ---------------------------------------------------------------------------*/
void test_bb_dispatch_cmd_call_ok_result_filled(void)
{
    bb_dispatch_cmd_test_reset();
    bb_dispatch_cmd_register("get_status", handler_ok, NULL);

    bb_json_t result = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_OK, bb_dispatch_cmd_call("get_status", NULL, result));

    bool handled = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(result, "handled", &handled));
    TEST_ASSERT_TRUE(handled);
    bb_json_free(result);
}

/* ---------------------------------------------------------------------------
 * call: same-length, different-content action names are not confused —
 * exercises the memcmp mismatch continue path in the lookup scan.
 * ---------------------------------------------------------------------------*/
void test_bb_dispatch_cmd_call_same_length_different_action_dispatches_correct_handler(void)
{
    bb_dispatch_cmd_test_reset();
    bb_dispatch_cmd_register("action_1", handler_fail, NULL);
    bb_dispatch_cmd_register("action_2", handler_ok, NULL);

    bb_json_t result = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_OK, bb_dispatch_cmd_call("action_2", NULL, result));

    bool handled = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(result, "handled", &handled));
    TEST_ASSERT_TRUE(handled);
    bb_json_free(result);
}

/* ---------------------------------------------------------------------------
 * call: handler error propagates
 * ---------------------------------------------------------------------------*/
void test_bb_dispatch_cmd_call_propagates_handler_error(void)
{
    bb_dispatch_cmd_test_reset();
    bb_dispatch_cmd_register("do_fail", handler_fail, NULL);

    bb_json_t result = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_dispatch_cmd_call("do_fail", NULL, result));
    bb_json_free(result);
}

/* ---------------------------------------------------------------------------
 * call: unknown action → NOT_FOUND
 * ---------------------------------------------------------------------------*/
void test_bb_dispatch_cmd_call_unknown_action_not_found(void)
{
    bb_dispatch_cmd_test_reset();
    bb_dispatch_cmd_register("get_status", handler_ok, NULL);

    bb_json_t result = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_dispatch_cmd_call("unknown", NULL, result));
    bb_json_free(result);
}

/* ---------------------------------------------------------------------------
 * call: null action / result_out → BB_ERR_INVALID_ARG
 * ---------------------------------------------------------------------------*/
void test_bb_dispatch_cmd_call_null_action_returns_invalid_arg(void)
{
    bb_dispatch_cmd_test_reset();
    bb_json_t result = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_dispatch_cmd_call(NULL, NULL, result));
    bb_json_free(result);
}

void test_bb_dispatch_cmd_call_null_result_out_returns_invalid_arg(void)
{
    bb_dispatch_cmd_test_reset();
    bb_dispatch_cmd_register("get_status", handler_ok, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_dispatch_cmd_call("get_status", NULL, NULL));
}

/* ---------------------------------------------------------------------------
 * authorizer: default NULL allows all
 * ---------------------------------------------------------------------------*/
void test_bb_dispatch_cmd_no_authorizer_allows_all(void)
{
    bb_dispatch_cmd_test_reset();
    s_handler_calls = 0;
    bb_dispatch_cmd_register("do_thing", handler_counting, NULL);

    bb_json_t result = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_OK, bb_dispatch_cmd_call("do_thing", NULL, result));
    TEST_ASSERT_EQUAL(1, s_handler_calls);
    bb_json_free(result);
}

/* ---------------------------------------------------------------------------
 * authorizer: allows call through
 * ---------------------------------------------------------------------------*/
void test_bb_dispatch_cmd_authorizer_allows_call(void)
{
    bb_dispatch_cmd_test_reset();
    s_handler_calls = 0;
    bb_dispatch_cmd_register("do_thing", handler_counting, NULL);
    bb_dispatch_cmd_set_authorizer(authorizer_allow, NULL);

    bb_json_t result = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_OK, bb_dispatch_cmd_call("do_thing", NULL, result));
    TEST_ASSERT_EQUAL(1, s_handler_calls);
    bb_json_free(result);
}

/* ---------------------------------------------------------------------------
 * authorizer: rejects call, handler never runs
 * ---------------------------------------------------------------------------*/
void test_bb_dispatch_cmd_authorizer_rejects_call(void)
{
    bb_dispatch_cmd_test_reset();
    s_handler_calls = 0;
    bb_dispatch_cmd_register("do_thing", handler_counting, NULL);
    bb_dispatch_cmd_set_authorizer(authorizer_reject, NULL);

    bb_json_t result = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_ERR_UNAUTHORIZED, bb_dispatch_cmd_call("do_thing", NULL, result));
    TEST_ASSERT_EQUAL(0, s_handler_calls);
    bb_json_free(result);
}

/* ---------------------------------------------------------------------------
 * authorizer reject (BB_ERR_UNAUTHORIZED) is distinguishable from a handler
 * that legitimately returns BB_ERR_INVALID_STATE — a WS consumer needs to
 * tell "unauthorized" apart from "handler internal error".
 * ---------------------------------------------------------------------------*/
void test_bb_dispatch_cmd_authorizer_reject_distinct_from_handler_invalid_state(void)
{
    bb_dispatch_cmd_test_reset();
    bb_dispatch_cmd_register("do_fail", handler_fail, NULL);
    bb_dispatch_cmd_set_authorizer(authorizer_allow, NULL);

    /* Handler-originated BB_ERR_INVALID_STATE passes through unchanged. */
    bb_json_t result = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_dispatch_cmd_call("do_fail", NULL, result));
    bb_json_free(result);

    /* Authorizer rejection is a distinct code, never confused with the above. */
    bb_dispatch_cmd_set_authorizer(authorizer_reject, NULL);
    bb_json_t result2 = bb_json_obj_new();
    bb_err_t rc = bb_dispatch_cmd_call("do_fail", NULL, result2);
    TEST_ASSERT_EQUAL(BB_ERR_UNAUTHORIZED, rc);
    TEST_ASSERT_NOT_EQUAL(BB_ERR_INVALID_STATE, rc);
    bb_json_free(result2);
}

/* ---------------------------------------------------------------------------
 * set_authorizer(NULL) clears back to allow-all
 * ---------------------------------------------------------------------------*/
void test_bb_dispatch_cmd_set_authorizer_null_clears_to_allow_all(void)
{
    bb_dispatch_cmd_test_reset();
    s_handler_calls = 0;
    bb_dispatch_cmd_register("do_thing", handler_counting, NULL);
    bb_dispatch_cmd_set_authorizer(authorizer_reject, NULL);
    bb_dispatch_cmd_set_authorizer(NULL, NULL);

    bb_json_t result = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_OK, bb_dispatch_cmd_call("do_thing", NULL, result));
    TEST_ASSERT_EQUAL(1, s_handler_calls);
    bb_json_free(result);
}

/* ---------------------------------------------------------------------------
 * test_reset: clears registered actions and authorizer
 * ---------------------------------------------------------------------------*/
void test_bb_dispatch_cmd_test_reset_clears_state(void)
{
    bb_dispatch_cmd_test_reset();
    bb_dispatch_cmd_register("get_status", handler_ok, NULL);
    bb_dispatch_cmd_set_authorizer(authorizer_reject, NULL);
    TEST_ASSERT_EQUAL(1, (int)bb_dispatch_cmd_count());

    bb_dispatch_cmd_test_reset();
    TEST_ASSERT_EQUAL(0, (int)bb_dispatch_cmd_count());

    /* Authorizer cleared too — a freshly registered action dispatches. */
    s_handler_calls = 0;
    bb_dispatch_cmd_register("get_status", handler_counting, NULL);
    bb_json_t result = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_OK, bb_dispatch_cmd_call("get_status", NULL, result));
    TEST_ASSERT_EQUAL(1, s_handler_calls);
    bb_json_free(result);

    /* Lookup of the pre-reset action is gone (NOT_FOUND, not stale data). */
    bb_dispatch_cmd_test_reset();
    bb_json_t result2 = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_dispatch_cmd_call("get_status", NULL, result2));
    bb_json_free(result2);
}

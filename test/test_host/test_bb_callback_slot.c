#include "unity.h"
#include "bb_callback_slot.h"

// setUp/tearDown: defined in test_main.c (global).
//
// This is the SINGLE place the bb_callback_slot invoke logic is tested —
// per-seam dispatch tests (e.g. the old bb_wifi_ota_validated_eval host
// tests) are redundant once a seam is migrated onto these macros.

// ---------------------------------------------------------------------------
// BB_CALLBACK_SLOT_VOID -- dummy void-fire callback WITH an argument.
// ---------------------------------------------------------------------------

typedef void (*cbslot_test_void_fn)(int val);

static int s_cbslot_void_last_a = -1;
static void cbslot_test_void_fixture_a(int val)
{
    s_cbslot_void_last_a = val;
}

static int s_cbslot_void_last_b = -1;
static void cbslot_test_void_fixture_b(int val)
{
    s_cbslot_void_last_b = val;
}

// Forward declarations of the generated public symbols (no undeclared
// external — mirrors the "callers declare in a header" contract for the
// real, component-side instantiations).
void cbslot_test_void_set(cbslot_test_void_fn cb);
void cbslot_test_void_invoke(int val);

BB_CALLBACK_SLOT_VOID(cbslot_test_void, cbslot_test_void_fn, cbslot_test_void_set,
                      cbslot_test_void_invoke, (int val), (val))

// Null slot -> no-op (no crash, cb not invoked).
void test_bb_callback_slot_void_null_slot_is_noop(void)
{
    s_cbslot_void_last_a = -1;
    cbslot_test_void_invoke(7);
    TEST_ASSERT_EQUAL_INT(-1, s_cbslot_void_last_a);
}

// Set slot -> invoke calls it, forwarding the argument.
void test_bb_callback_slot_void_set_slot_dispatches_with_arg(void)
{
    cbslot_test_void_set(cbslot_test_void_fixture_a);
    s_cbslot_void_last_a = -1;
    cbslot_test_void_invoke(42);
    TEST_ASSERT_EQUAL_INT(42, s_cbslot_void_last_a);
    cbslot_test_void_set(NULL);
}

// Re-set replaces: invoke after a second cbslot_test_void_set call fires the
// NEW callback only.
void test_bb_callback_slot_void_re_set_replaces(void)
{
    cbslot_test_void_set(cbslot_test_void_fixture_a);
    cbslot_test_void_set(cbslot_test_void_fixture_b);
    s_cbslot_void_last_a = -1;
    s_cbslot_void_last_b = -1;
    cbslot_test_void_invoke(9);
    TEST_ASSERT_EQUAL_INT(-1, s_cbslot_void_last_a);
    TEST_ASSERT_EQUAL_INT(9, s_cbslot_void_last_b);
    cbslot_test_void_set(NULL);
}

// ---------------------------------------------------------------------------
// BB_CALLBACK_SLOT_RET -- dummy value-return-with-default callback.
// ---------------------------------------------------------------------------

typedef int (*cbslot_test_ret_fn)(void);

static int cbslot_test_ret_fixture_a(void) { return 99; }
static int cbslot_test_ret_fixture_b(void) { return 7; }

void cbslot_test_ret_set(cbslot_test_ret_fn cb);
int  cbslot_test_ret_invoke(void);

BB_CALLBACK_SLOT_RET(cbslot_test_ret, cbslot_test_ret_fn, int, cbslot_test_ret_set,
                     cbslot_test_ret_invoke, -1)

// Null slot -> default.
void test_bb_callback_slot_ret_null_slot_returns_default(void)
{
    cbslot_test_ret_set(NULL);
    TEST_ASSERT_EQUAL_INT(-1, cbslot_test_ret_invoke());
}

// Set slot -> invoke returns the callback's value.
void test_bb_callback_slot_ret_set_slot_returns_cb_value(void)
{
    cbslot_test_ret_set(cbslot_test_ret_fixture_a);
    TEST_ASSERT_EQUAL_INT(99, cbslot_test_ret_invoke());
    cbslot_test_ret_set(NULL);
}

// Re-set replaces: invoke after a second set call returns the NEW
// callback's value.
void test_bb_callback_slot_ret_re_set_replaces(void)
{
    cbslot_test_ret_set(cbslot_test_ret_fixture_a);
    cbslot_test_ret_set(cbslot_test_ret_fixture_b);
    TEST_ASSERT_EQUAL_INT(7, cbslot_test_ret_invoke());
    cbslot_test_ret_set(NULL);
}

// ---------------------------------------------------------------------------
// BB_CALLBACK_SLOT_VOID0 -- dummy void-fire, NO-ARG callback (the previously
// broken path: routing a no-arg callback through the with-args EXPAND_
// machinery). This is the dedicated no-arg macro that replaces it.
// ---------------------------------------------------------------------------

typedef void (*cbslot_test_void0_fn)(void);

static int s_cbslot_void0_calls_a = 0;
static void cbslot_test_void0_fixture_a(void)
{
    s_cbslot_void0_calls_a++;
}

static int s_cbslot_void0_calls_b = 0;
static void cbslot_test_void0_fixture_b(void)
{
    s_cbslot_void0_calls_b++;
}

void cbslot_test_void0_set(cbslot_test_void0_fn cb);
void cbslot_test_void0_invoke(void);

BB_CALLBACK_SLOT_VOID0(cbslot_test_void0, cbslot_test_void0_fn, cbslot_test_void0_set,
                       cbslot_test_void0_invoke)

// Null slot -> no-op (no crash, cb not invoked).
void test_bb_callback_slot_void0_null_slot_is_noop(void)
{
    s_cbslot_void0_calls_a = 0;
    cbslot_test_void0_invoke();
    TEST_ASSERT_EQUAL_INT(0, s_cbslot_void0_calls_a);
}

// Set slot -> invoke calls it with no arguments.
void test_bb_callback_slot_void0_set_slot_dispatches(void)
{
    cbslot_test_void0_set(cbslot_test_void0_fixture_a);
    s_cbslot_void0_calls_a = 0;
    cbslot_test_void0_invoke();
    TEST_ASSERT_EQUAL_INT(1, s_cbslot_void0_calls_a);
    cbslot_test_void0_set(NULL);
}

// Re-set replaces: invoke after a second set call fires the NEW callback only.
void test_bb_callback_slot_void0_re_set_replaces(void)
{
    cbslot_test_void0_set(cbslot_test_void0_fixture_a);
    cbslot_test_void0_set(cbslot_test_void0_fixture_b);
    s_cbslot_void0_calls_a = 0;
    s_cbslot_void0_calls_b = 0;
    cbslot_test_void0_invoke();
    TEST_ASSERT_EQUAL_INT(0, s_cbslot_void0_calls_a);
    TEST_ASSERT_EQUAL_INT(1, s_cbslot_void0_calls_b);
    cbslot_test_void0_set(NULL);
}

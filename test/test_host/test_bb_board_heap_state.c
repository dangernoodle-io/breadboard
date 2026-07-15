// Host tests for bb_board's heap-state classifier — moved from
// test_bb_net_health.c (net_health teardown PR-C).
#include "unity.h"
#include "bb_board.h"

// ---------------------------------------------------------------------------
// bb_board_classify_heap — pure bucket classifier (moved from
// bb_net_health_classify_heap, B1-439)
// ---------------------------------------------------------------------------

// Heap well above both thresholds → OK.
void test_bb_board_classify_heap_ok(void)
{
    bb_board_heap_state_t s = bb_board_classify_heap(BB_BOARD_HEAP_LOW_BYTES + 1);
    TEST_ASSERT_EQUAL_INT(BB_BOARD_HEAP_STATE_OK, s);
}

// Heap between CRITICAL and LOW → LOW.
void test_bb_board_classify_heap_low(void)
{
    size_t mid = (BB_BOARD_HEAP_CRITICAL_BYTES + BB_BOARD_HEAP_LOW_BYTES) / 2;
    bb_board_heap_state_t s = bb_board_classify_heap(mid);
    TEST_ASSERT_EQUAL_INT(BB_BOARD_HEAP_STATE_LOW, s);
}

// Heap below CRITICAL → CRITICAL.
void test_bb_board_classify_heap_critical(void)
{
    bb_board_heap_state_t s = bb_board_classify_heap(BB_BOARD_HEAP_CRITICAL_BYTES - 1);
    TEST_ASSERT_EQUAL_INT(BB_BOARD_HEAP_STATE_CRITICAL, s);
}

// Zero free bytes is also CRITICAL.
void test_bb_board_classify_heap_zero(void)
{
    bb_board_heap_state_t s = bb_board_classify_heap(0);
    TEST_ASSERT_EQUAL_INT(BB_BOARD_HEAP_STATE_CRITICAL, s);
}

// ---------------------------------------------------------------------------
// bb_board_heap_state_str — string helper, including the default/unknown
// branch (moved from bb_heap_state_str)
// ---------------------------------------------------------------------------

void test_bb_board_heap_state_str_ok(void)
{
    TEST_ASSERT_EQUAL_STRING("ok", bb_board_heap_state_str(BB_BOARD_HEAP_STATE_OK));
}

void test_bb_board_heap_state_str_low(void)
{
    TEST_ASSERT_EQUAL_STRING("low", bb_board_heap_state_str(BB_BOARD_HEAP_STATE_LOW));
}

void test_bb_board_heap_state_str_critical(void)
{
    TEST_ASSERT_EQUAL_STRING("critical", bb_board_heap_state_str(BB_BOARD_HEAP_STATE_CRITICAL));
}

// Cast an out-of-range value to exercise the default branch.
void test_bb_board_heap_state_str_unknown_returns_ok(void)
{
    const char *s = bb_board_heap_state_str((bb_board_heap_state_t)99);
    TEST_ASSERT_EQUAL_STRING("ok", s);
}

// ---------------------------------------------------------------------------
// bb_board_set_heap_state / bb_board_heap_state
//
// bb_board_set_heap_state is an internal setter not declared in the public
// header; the ESP-IDF platform file (bb_net_health_espidf.c) forward-declares
// it with extern. We use the same pattern here to cover the three lines
// (signature, body, closing brace) that were the only uncovered lines in the
// module (moved from bb_net_health_set_heap_state).
// ---------------------------------------------------------------------------

// Internal setter, matching the forward-declare pattern used by
// platform/espidf/bb_net_health/bb_net_health_espidf.c.
extern void bb_board_set_heap_state(bb_board_heap_state_t state);

// Round-trip: set each non-default state and read it back via the public getter.
void test_bb_board_set_heap_state_roundtrip(void)
{
    // Initial value must be OK (zero-init module static).
    TEST_ASSERT_EQUAL_INT(BB_BOARD_HEAP_STATE_OK, bb_board_heap_state());

    bb_board_set_heap_state(BB_BOARD_HEAP_STATE_LOW);
    TEST_ASSERT_EQUAL_INT(BB_BOARD_HEAP_STATE_LOW, bb_board_heap_state());

    bb_board_set_heap_state(BB_BOARD_HEAP_STATE_CRITICAL);
    TEST_ASSERT_EQUAL_INT(BB_BOARD_HEAP_STATE_CRITICAL, bb_board_heap_state());

    // Restore to OK so other tests are not affected by residual state.
    bb_board_set_heap_state(BB_BOARD_HEAP_STATE_OK);
    TEST_ASSERT_EQUAL_INT(BB_BOARD_HEAP_STATE_OK, bb_board_heap_state());
}

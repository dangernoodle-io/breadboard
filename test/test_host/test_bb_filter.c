// Host tests for bb_filter — pure projection over bb_attrs elements.
// Injection-first: fake bb_attrs arrays x selectors, assert exact
// subset/order/count. Covers every gate, the pressure-shed path
// (DEFERRABLE-before-MUST), the emit_decide FSM, and edge cases (empty
// input, out_cap < filtered count, NULL args).
#include "unity.h"
#include "bb_filter.h"

#include <string.h>

static bb_attrs_t bb_filter_test_mk(uint8_t priority, uint16_t kind, uint32_t tag_mask, uint8_t delivery_class)
{
    bb_attrs_t a;
    memset(&a, 0, sizeof(a));
    a.priority       = priority;
    a.kind           = kind;
    a.tag_mask       = tag_mask;
    a.delivery_class = delivery_class;
    return a;
}

static bb_filter_selector_t bb_filter_test_sel_default(void)
{
    bb_filter_selector_t sel;
    memset(&sel, 0, sizeof(sel));
    sel.priority_max = 0xFF;
    return sel;
}

// ---------------------------------------------------------------------------
// bb_filter_select — basic ordering / stability
// ---------------------------------------------------------------------------

void test_bb_filter_select_orders_by_priority_ascending(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(5, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_attrs_t a1 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_attrs_t a2 = bb_filter_test_mk(3, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[3] = {
        { .attrs = &a0, .item = NULL },
        { .attrs = &a1, .item = NULL },
        { .attrs = &a2, .item = NULL },
    };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    bb_filter_elem_t out[3];

    size_t n = bb_filter_select(in, 3, &sel, out, 3);

    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_PTR(&a1, out[0].attrs);
    TEST_ASSERT_EQUAL_PTR(&a2, out[1].attrs);
    TEST_ASSERT_EQUAL_PTR(&a0, out[2].attrs);
}

void test_bb_filter_select_stable_ties_keep_input_order(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(2, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_attrs_t a1 = bb_filter_test_mk(2, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_attrs_t a2 = bb_filter_test_mk(2, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[3] = {
        { .attrs = &a0, .item = NULL },
        { .attrs = &a1, .item = NULL },
        { .attrs = &a2, .item = NULL },
    };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    bb_filter_elem_t out[3];

    size_t n = bb_filter_select(in, 3, &sel, out, 3);

    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_PTR(&a0, out[0].attrs);
    TEST_ASSERT_EQUAL_PTR(&a1, out[1].attrs);
    TEST_ASSERT_EQUAL_PTR(&a2, out[2].attrs);
}

// ---------------------------------------------------------------------------
// bb_filter_select — gates
// ---------------------------------------------------------------------------

void test_bb_filter_select_priority_max_excludes_worse(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_attrs_t a1 = bb_filter_test_mk(9, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[2] = {
        { .attrs = &a0, .item = NULL },
        { .attrs = &a1, .item = NULL },
    };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.priority_max = 5;
    bb_filter_elem_t out[2];

    size_t n = bb_filter_select(in, 2, &sel, out, 2);

    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_PTR(&a0, out[0].attrs);
}

void test_bb_filter_select_priority_max_0xff_admits_all(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(255, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    bb_filter_elem_t out[1];

    size_t n = bb_filter_select(in, 1, &sel, out, 1);

    TEST_ASSERT_EQUAL_size_t(1, n);
}

void test_bb_filter_select_kind_mask_zero_admits_all_kinds(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 7, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    bb_filter_elem_t out[1];

    size_t n = bb_filter_select(in, 1, &sel, out, 1);

    TEST_ASSERT_EQUAL_size_t(1, n);
}

void test_bb_filter_select_kind_mask_matches_bit(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 2, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.kind_mask = (1u << 2);
    bb_filter_elem_t out[1];

    size_t n = bb_filter_select(in, 1, &sel, out, 1);

    TEST_ASSERT_EQUAL_size_t(1, n);
}

void test_bb_filter_select_kind_mask_excludes_unset_bit(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 3, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.kind_mask = (1u << 2);
    bb_filter_elem_t out[1];

    size_t n = bb_filter_select(in, 1, &sel, out, 1);

    TEST_ASSERT_EQUAL_size_t(0, n);
}

void test_bb_filter_select_kind_ge_16_excluded_when_mask_set(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 16, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.kind_mask = 0xFFFF;
    bb_filter_elem_t out[1];

    size_t n = bb_filter_select(in, 1, &sel, out, 1);

    TEST_ASSERT_EQUAL_size_t(0, n);
}

void test_bb_filter_select_tag_mask_zero_admits_all(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0x1, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    bb_filter_elem_t out[1];

    size_t n = bb_filter_select(in, 1, &sel, out, 1);

    TEST_ASSERT_EQUAL_size_t(1, n);
}

void test_bb_filter_select_tag_mask_requires_overlap(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0x2, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.tag_mask = 0x1;
    bb_filter_elem_t out[1];

    size_t n = bb_filter_select(in, 1, &sel, out, 1);

    TEST_ASSERT_EQUAL_size_t(0, n);
}

void test_bb_filter_select_tag_mask_overlap_included(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0x3, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.tag_mask = 0x1;
    bb_filter_elem_t out[1];

    size_t n = bb_filter_select(in, 1, &sel, out, 1);

    TEST_ASSERT_EQUAL_size_t(1, n);
}

void test_bb_filter_select_min_delivery_excludes_must(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_attrs_t a1 = bb_filter_test_mk(2, 0, 0, BB_ATTRS_DELIVERY_DEFERRABLE);
    bb_filter_elem_t in[2] = {
        { .attrs = &a0, .item = NULL },
        { .attrs = &a1, .item = NULL },
    };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.min_delivery = BB_ATTRS_DELIVERY_DEFERRABLE;
    bb_filter_elem_t out[2];

    size_t n = bb_filter_select(in, 2, &sel, out, 2);

    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_PTR(&a1, out[0].attrs);
}

void test_bb_filter_select_min_delivery_zero_admits_all(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    bb_filter_elem_t out[1];

    size_t n = bb_filter_select(in, 1, &sel, out, 1);

    TEST_ASSERT_EQUAL_size_t(1, n);
}

void test_bb_filter_select_null_attrs_element_skipped(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[2] = {
        { .attrs = NULL, .item = NULL },
        { .attrs = &a0, .item = NULL },
    };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    bb_filter_elem_t out[2];

    size_t n = bb_filter_select(in, 2, &sel, out, 2);

    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_PTR(&a0, out[0].attrs);
}

// ---------------------------------------------------------------------------
// bb_filter_select — max_count / out_cap truncation
// ---------------------------------------------------------------------------

void test_bb_filter_select_max_count_truncates(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_attrs_t a1 = bb_filter_test_mk(2, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_attrs_t a2 = bb_filter_test_mk(3, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[3] = {
        { .attrs = &a0, .item = NULL },
        { .attrs = &a1, .item = NULL },
        { .attrs = &a2, .item = NULL },
    };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.max_count = 2;
    bb_filter_elem_t out[3];

    size_t n = bb_filter_select(in, 3, &sel, out, 3);

    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_PTR(&a0, out[0].attrs);
    TEST_ASSERT_EQUAL_PTR(&a1, out[1].attrs);
}

void test_bb_filter_select_max_count_zero_unbounded(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_attrs_t a1 = bb_filter_test_mk(2, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[2] = {
        { .attrs = &a0, .item = NULL },
        { .attrs = &a1, .item = NULL },
    };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    bb_filter_elem_t out[2];

    size_t n = bb_filter_select(in, 2, &sel, out, 2);

    TEST_ASSERT_EQUAL_size_t(2, n);
}

void test_bb_filter_select_out_cap_smaller_than_filtered_count(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_attrs_t a1 = bb_filter_test_mk(2, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_attrs_t a2 = bb_filter_test_mk(3, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[3] = {
        { .attrs = &a0, .item = NULL },
        { .attrs = &a1, .item = NULL },
        { .attrs = &a2, .item = NULL },
    };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    bb_filter_elem_t out[1];

    size_t n = bb_filter_select(in, 3, &sel, out, 1);

    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_PTR(&a0, out[0].attrs);
}

// ---------------------------------------------------------------------------
// bb_filter_select — NULL / empty edge cases
// ---------------------------------------------------------------------------

void test_bb_filter_select_null_sel_returns_zero(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_elem_t out[1];

    TEST_ASSERT_EQUAL_size_t(0, bb_filter_select(in, 1, NULL, out, 1));
}

void test_bb_filter_select_null_out_returns_zero(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();

    TEST_ASSERT_EQUAL_size_t(0, bb_filter_select(in, 1, &sel, NULL, 1));
}

void test_bb_filter_select_zero_out_cap_returns_zero(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    bb_filter_elem_t out[1];

    TEST_ASSERT_EQUAL_size_t(0, bb_filter_select(in, 1, &sel, out, 0));
}

void test_bb_filter_select_null_in_returns_zero(void)
{
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    bb_filter_elem_t out[1];

    TEST_ASSERT_EQUAL_size_t(0, bb_filter_select(NULL, 1, &sel, out, 1));
}

void test_bb_filter_select_zero_n_returns_zero(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    bb_filter_elem_t out[1];

    TEST_ASSERT_EQUAL_size_t(0, bb_filter_select(in, 0, &sel, out, 1));
}

void test_bb_filter_select_all_gated_out_returns_zero(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(9, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.priority_max = 1;
    bb_filter_elem_t out[1];

    size_t n = bb_filter_select(in, 1, &sel, out, 1);

    TEST_ASSERT_EQUAL_size_t(0, n);
}

// ---------------------------------------------------------------------------
// bb_filter_select — pressure shedding (DEFERRABLE-before-MUST)
// ---------------------------------------------------------------------------

void test_bb_filter_select_pressure_zero_sheds_nothing(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_DEFERRABLE);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    bb_filter_elem_t out[1];

    size_t n = bb_filter_select(in, 1, &sel, out, 1);

    TEST_ASSERT_EQUAL_size_t(1, n);
}

void test_bb_filter_select_pressure_max_sheds_all_deferrable_never_must(void)
{
    bb_attrs_t must  = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_attrs_t defer = bb_filter_test_mk(2, 0, 0, BB_ATTRS_DELIVERY_DEFERRABLE);
    bb_filter_elem_t in[2] = {
        { .attrs = &must,  .item = NULL },
        { .attrs = &defer, .item = NULL },
    };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.pressure = 255;
    bb_filter_elem_t out[2];

    size_t n = bb_filter_select(in, 2, &sel, out, 2);

    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_PTR(&must, out[0].attrs);
}

void test_bb_filter_select_pressure_sheds_tail_deferrable_keeps_head(void)
{
    // Two DEFERRABLE elements at different priorities: moderate pressure
    // should shed the worst-priority (tail) one and keep the better one.
    bb_attrs_t best  = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_DEFERRABLE);
    bb_attrs_t worst = bb_filter_test_mk(2, 0, 0, BB_ATTRS_DELIVERY_DEFERRABLE);
    bb_filter_elem_t in[2] = {
        { .attrs = &best,  .item = NULL },
        { .attrs = &worst, .item = NULL },
    };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.pressure = 200; // (2 * 200) / 255 == 1 -> shed exactly one
    bb_filter_elem_t out[2];

    size_t n = bb_filter_select(in, 2, &sel, out, 2);

    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_PTR(&best, out[0].attrs);
}

void test_bb_filter_select_pressure_with_no_deferrable_is_noop(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.pressure = 255;
    bb_filter_elem_t out[1];

    size_t n = bb_filter_select(in, 1, &sel, out, 1);

    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_PTR(&a0, out[0].attrs);
}

void test_bb_filter_select_pressure_low_shed_count_zero_keeps_all(void)
{
    // (1 deferrable * pressure=1) / 255 == 0 -> nothing shed.
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_DEFERRABLE);
    bb_filter_elem_t in[1] = { { .attrs = &a0, .item = NULL } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.pressure = 1;
    bb_filter_elem_t out[1];

    size_t n = bb_filter_select(in, 1, &sel, out, 1);

    TEST_ASSERT_EQUAL_size_t(1, n);
}

// ---------------------------------------------------------------------------
// bb_attrs_container_of round trip through bb_filter_elem_t
// ---------------------------------------------------------------------------

typedef struct {
    bb_attrs_t attrs;
    int        value;
} bb_filter_test_owned_t;

void test_bb_filter_select_container_of_recovers_owner(void)
{
    bb_filter_test_owned_t owner = { .value = 99 };
    owner.attrs = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_elem_t in[1] = { { .attrs = &owner.attrs, .item = &owner } };
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    bb_filter_elem_t out[1];

    size_t n = bb_filter_select(in, 1, &sel, out, 1);

    TEST_ASSERT_EQUAL_size_t(1, n);
    bb_filter_test_owned_t *recovered =
        bb_attrs_container_of(out[0].attrs, bb_filter_test_owned_t, attrs);
    TEST_ASSERT_EQUAL_INT(99, recovered->value);
}

// ---------------------------------------------------------------------------
// bb_filter_emit_decide — FSM
// ---------------------------------------------------------------------------

void test_bb_filter_emit_decide_null_attrs_is_now(void)
{
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.pressure = 100;
    TEST_ASSERT_EQUAL_INT(BB_FILTER_EMIT_NOW, bb_filter_emit_decide(NULL, &sel, 500));
}

void test_bb_filter_emit_decide_null_sel_is_now(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_DEFERRABLE);
    TEST_ASSERT_EQUAL_INT(BB_FILTER_EMIT_NOW, bb_filter_emit_decide(&a0, NULL, 500));
}

void test_bb_filter_emit_decide_zero_pressure_is_now(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_DEFERRABLE);
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    TEST_ASSERT_EQUAL_INT(BB_FILTER_EMIT_NOW, bb_filter_emit_decide(&a0, &sel, 0));
}

void test_bb_filter_emit_decide_must_zero_since_last_is_defer(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.pressure = 50;
    TEST_ASSERT_EQUAL_INT(BB_FILTER_EMIT_DEFER, bb_filter_emit_decide(&a0, &sel, 0));
}

void test_bb_filter_emit_decide_must_nonzero_since_last_is_now(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_MUST);
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.pressure = 50;
    TEST_ASSERT_EQUAL_INT(BB_FILTER_EMIT_NOW, bb_filter_emit_decide(&a0, &sel, 1));
}

void test_bb_filter_emit_decide_deferrable_below_floor_is_defer(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_DEFERRABLE);
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.pressure = 10; // floor = 1000 ms
    TEST_ASSERT_EQUAL_INT(BB_FILTER_EMIT_DEFER, bb_filter_emit_decide(&a0, &sel, 500));
}

void test_bb_filter_emit_decide_deferrable_at_floor_is_now(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_DEFERRABLE);
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.pressure = 10; // floor = 1000 ms
    TEST_ASSERT_EQUAL_INT(BB_FILTER_EMIT_NOW, bb_filter_emit_decide(&a0, &sel, 1000));
}

void test_bb_filter_emit_decide_deferrable_below_double_floor_is_now(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_DEFERRABLE);
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.pressure = 10; // floor = 1000 ms, double = 2000 ms
    TEST_ASSERT_EQUAL_INT(BB_FILTER_EMIT_NOW, bb_filter_emit_decide(&a0, &sel, 1999));
}

void test_bb_filter_emit_decide_deferrable_at_double_floor_is_drop(void)
{
    bb_attrs_t a0 = bb_filter_test_mk(1, 0, 0, BB_ATTRS_DELIVERY_DEFERRABLE);
    bb_filter_selector_t sel = bb_filter_test_sel_default();
    sel.pressure = 10; // floor = 1000 ms, double = 2000 ms
    TEST_ASSERT_EQUAL_INT(BB_FILTER_EMIT_DROP, bb_filter_emit_decide(&a0, &sel, 2000));
}

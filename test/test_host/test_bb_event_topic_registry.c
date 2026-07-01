// Host tests for bb_event_topic_registry — thin bb_registry consumer backing
// bb_event_routes_attach_ex2/capture_cb/topic_count/topic_info.
//
// Coverage targets: register dedupe-on-duplicate-name -> BB_OK, overflow ->
// BB_ERR_NO_SPACE, register NULL args, find_by_handle hit/miss/NULL args,
// count, get_by_index (hit + out-of-range + NULL out), test_reset.
//
// Every test ends by resetting the registry back to empty. This registry is
// shared with bb_event_routes_common.c, and the global setUp() (test_main.c)
// calls bb_event_routes_reset_for_test() before EVERY test in the whole
// binary — including tests unrelated to this file — which walks the
// registry and calls bb_event_ring_detach() on any non-NULL `ring` field it
// finds. Leaving a fake/non-dereferenceable ring pointer registered here
// would crash that unrelated test's setUp().

#include "unity.h"
#include "bb_event.h"
#include "bb_event_ring.h"
#include "bb_event_topic_registry.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bb_event_attached_topic_t s_slots[40];

static bb_event_attached_topic_t *make_slot(size_t idx, const char *name,
                                            bb_event_topic_t topic,
                                            bb_event_ring_t ring)
{
    bb_event_attached_topic_t *t = &s_slots[idx];
    strncpy(t->name, name, BB_EVENT_TOPIC_NAME_MAX - 1);
    t->name[BB_EVENT_TOPIC_NAME_MAX - 1] = '\0';
    t->topic = topic;
    t->ring = ring;
    return t;
}

// ---------------------------------------------------------------------------
// register
// ---------------------------------------------------------------------------

void test_bb_event_topic_registry_register_and_count(void)
{
    bb_event_topic_registry_test_reset();

    bb_event_topic_t topic_a = (bb_event_topic_t)(void *)0x1;
    bb_event_attached_topic_t *t = make_slot(0, "topic.a", topic_a, NULL);

    TEST_ASSERT_EQUAL(BB_OK, bb_event_topic_registry_register(t->name, t));
    TEST_ASSERT_EQUAL_size_t(1, bb_event_topic_registry_count());

    bb_event_topic_registry_test_reset();
}

void test_bb_event_topic_registry_register_null_args_returns_invalid_arg(void)
{
    bb_event_topic_registry_test_reset();
    bb_event_attached_topic_t *t = make_slot(0, "topic.b", (bb_event_topic_t)(void *)0x2, NULL);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_event_topic_registry_register(NULL, t));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_event_topic_registry_register("x", NULL));

    bb_event_topic_registry_test_reset();
}

// Duplicate name -> BB_OK (idempotent), no second entry committed.
void test_bb_event_topic_registry_register_duplicate_name_returns_ok(void)
{
    bb_event_topic_registry_test_reset();
    bb_event_topic_t topic_a = (bb_event_topic_t)(void *)0x1;
    bb_event_attached_topic_t *t1 = make_slot(0, "dup.topic", topic_a, NULL);
    bb_event_attached_topic_t *t2 = make_slot(1, "dup.topic", topic_a, NULL);

    TEST_ASSERT_EQUAL(BB_OK, bb_event_topic_registry_register(t1->name, t1));
    TEST_ASSERT_EQUAL(BB_OK, bb_event_topic_registry_register(t2->name, t2));
    TEST_ASSERT_EQUAL_size_t(1, bb_event_topic_registry_count());

    bb_event_topic_registry_test_reset();
}

void test_bb_event_topic_registry_register_overflow_returns_no_space(void)
{
    bb_event_topic_registry_test_reset();

    char names[40][16];
    bb_err_t err = BB_OK;
    size_t registered = 0;
    for (int i = 0; i < 40; i++) {
        snprintf(names[i], sizeof names[i], "t%d", i);
        bb_event_topic_t topic = (bb_event_topic_t)(void *)(uintptr_t)(i + 1);
        bb_event_attached_topic_t *t = make_slot((size_t)i, names[i], topic, NULL);
        err = bb_event_topic_registry_register(t->name, t);
        if (err != BB_OK) {
            break;
        }
        registered++;
    }

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_TRUE(registered > 0);
    TEST_ASSERT_EQUAL_size_t(registered, bb_event_topic_registry_count());

    bb_event_topic_registry_test_reset();
}

// ---------------------------------------------------------------------------
// find_by_handle
// ---------------------------------------------------------------------------

void test_bb_event_topic_registry_find_by_handle_hit(void)
{
    bb_event_topic_registry_test_reset();
    bb_event_topic_t topic_a = (bb_event_topic_t)(void *)0x1;
    bb_event_topic_t topic_b = (bb_event_topic_t)(void *)0x2;
    bb_event_attached_topic_t *ta = make_slot(0, "find.a", topic_a, NULL);
    bb_event_attached_topic_t *tb = make_slot(1, "find.b", topic_b, NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_event_topic_registry_register(ta->name, ta));
    TEST_ASSERT_EQUAL(BB_OK, bb_event_topic_registry_register(tb->name, tb));

    size_t idx = 999;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_topic_registry_find_by_handle(topic_b, &idx));
    TEST_ASSERT_EQUAL_size_t(1, idx);

    bb_event_topic_registry_test_reset();
}

void test_bb_event_topic_registry_find_by_handle_miss(void)
{
    bb_event_topic_registry_test_reset();
    bb_event_topic_t topic_a = (bb_event_topic_t)(void *)0x1;
    bb_event_attached_topic_t *ta = make_slot(0, "find.miss", topic_a, NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_event_topic_registry_register(ta->name, ta));

    size_t idx = 0;
    bb_event_topic_t unknown = (bb_event_topic_t)(void *)0xDEAD;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_event_topic_registry_find_by_handle(unknown, &idx));

    bb_event_topic_registry_test_reset();
}

void test_bb_event_topic_registry_find_by_handle_null_args_returns_invalid_arg(void)
{
    bb_event_topic_registry_test_reset();
    size_t idx = 0;
    bb_event_topic_t topic_a = (bb_event_topic_t)(void *)0x1;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_event_topic_registry_find_by_handle(NULL, &idx));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_event_topic_registry_find_by_handle(topic_a, NULL));

    bb_event_topic_registry_test_reset();
}

// ---------------------------------------------------------------------------
// get_by_index
// ---------------------------------------------------------------------------

void test_bb_event_topic_registry_get_by_index_hit(void)
{
    bb_event_topic_registry_test_reset();
    bb_event_topic_t topic_a = (bb_event_topic_t)(void *)0x1;
    // NULL ring: bb_event_routes_reset_for_test() (run by the global setUp()
    // before EVERY test in the binary) walks this shared registry and calls
    // bb_event_ring_detach() on any non-NULL ring it finds — a fake
    // non-dereferenceable ring pointer left registered here would crash an
    // unrelated later test. NULL is skipped by that cleanup, and this test
    // still exercises get_by_index's full field round-trip.
    bb_event_attached_topic_t *ta = make_slot(0, "idx.a", topic_a, NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_event_topic_registry_register(ta->name, ta));

    bb_event_attached_topic_t *out = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_topic_registry_get_by_index(0, &out));
    TEST_ASSERT_EQUAL_PTR(ta, out);
    TEST_ASSERT_EQUAL_STRING("idx.a", out->name);
    TEST_ASSERT_NULL(out->ring);

    bb_event_topic_registry_test_reset();
}

void test_bb_event_topic_registry_get_by_index_out_of_range_returns_not_found(void)
{
    bb_event_topic_registry_test_reset();
    bb_event_attached_topic_t *out = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_event_topic_registry_get_by_index(0, &out));
}

void test_bb_event_topic_registry_get_by_index_null_out_returns_invalid_arg(void)
{
    bb_event_topic_registry_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_event_topic_registry_get_by_index(0, NULL));
}

// idx > UINT16_MAX must not truncate to a valid uint16_t slot.
void test_bb_event_topic_registry_get_by_index_huge_idx_returns_not_found(void)
{
    bb_event_topic_registry_test_reset();
    bb_event_attached_topic_t *out = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_event_topic_registry_get_by_index(70000, &out));

    bb_event_topic_registry_test_reset();
}

// ---------------------------------------------------------------------------
// test_reset
// ---------------------------------------------------------------------------

void test_bb_event_topic_registry_test_reset_clears_all(void)
{
    bb_event_topic_registry_test_reset();
    bb_event_topic_t topic_a = (bb_event_topic_t)(void *)0x1;
    bb_event_attached_topic_t *ta = make_slot(0, "reset.a", topic_a, NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_event_topic_registry_register(ta->name, ta));
    TEST_ASSERT_EQUAL_size_t(1, bb_event_topic_registry_count());

    bb_event_topic_registry_test_reset();
    TEST_ASSERT_EQUAL_size_t(0, bb_event_topic_registry_count());
}

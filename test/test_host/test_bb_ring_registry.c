// Host tests for bb_ring_registry — thin bb_registry consumer tracking live
// bb_ring_t handles for GET /api/diag/rings.
//
// Coverage targets: register/deregister roundtrip, overflow, create/destroy
// auto-(de)registration (including B1-419 destroy->recreate), bb_ring_capacity,
// foreach, and test_reset.

#include "unity.h"
#include "bb_ring.h"
#include "bb_ring_registry.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bb_ring_t make_ring(const char *name)
{
    bb_ring_t r = NULL;
    bb_err_t err = bb_ring_create(1, 1, BB_RING_EVICT_OLDEST, name, &r);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_NOT_NULL(r);
    return r;
}

typedef struct {
    int       calls;
    char      last_name[32];
    bb_ring_t last_r;
} foreach_capture_t;

static void foreach_capture_cb(const char *name, bb_ring_t r, void *ctx)
{
    foreach_capture_t *cap = (foreach_capture_t *)ctx;
    cap->calls++;
    strncpy(cap->last_name, name, sizeof(cap->last_name) - 1);
    cap->last_name[sizeof(cap->last_name) - 1] = '\0';
    cap->last_r = r;
}

// ---------------------------------------------------------------------------
// register / deregister roundtrip (direct API, distinct fake handles)
// ---------------------------------------------------------------------------

void test_bb_ring_registry_register_deregister_roundtrip(void)
{
    bb_ring_registry_test_reset();

    int dummy = 0;
    bb_ring_t fake = (bb_ring_t)(void *)&dummy;

    TEST_ASSERT_EQUAL(BB_OK, bb_ring_registry_register("rt", fake));
    TEST_ASSERT_EQUAL_UINT16(1, bb_ring_registry_count());

    TEST_ASSERT_EQUAL(BB_OK, bb_ring_registry_deregister(fake));
    TEST_ASSERT_EQUAL_UINT16(0, bb_ring_registry_count());

    // deregistering an already-removed handle is BB_ERR_NOT_FOUND, not a crash.
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_ring_registry_deregister(fake));
}

// Deregister-by-value must remove the CORRECT entry when multiple rings are
// registered concurrently (guards the by-value scan / TOCTOU fix).
void test_bb_ring_registry_deregister_by_value_removes_correct_entry(void)
{
    bb_ring_registry_test_reset();

    int a = 0, b = 0;
    bb_ring_t ra = (bb_ring_t)(void *)&a;
    bb_ring_t rb = (bb_ring_t)(void *)&b;

    TEST_ASSERT_EQUAL(BB_OK, bb_ring_registry_register("ring_a", ra));
    TEST_ASSERT_EQUAL(BB_OK, bb_ring_registry_register("ring_b", rb));
    TEST_ASSERT_EQUAL_UINT16(2, bb_ring_registry_count());

    TEST_ASSERT_EQUAL(BB_OK, bb_ring_registry_deregister(ra));

    TEST_ASSERT_EQUAL_UINT16(1, bb_ring_registry_count());

    // A is gone; a second deregister on it is BB_ERR_NOT_FOUND.
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_ring_registry_deregister(ra));

    // B is still present and still resolvable by value.
    foreach_capture_t cap = { 0 };
    bb_ring_registry_foreach(foreach_capture_cb, &cap);
    TEST_ASSERT_EQUAL(1, cap.calls);
    TEST_ASSERT_EQUAL_STRING("ring_b", cap.last_name);
    TEST_ASSERT_EQUAL_PTR(rb, cap.last_r);

    TEST_ASSERT_EQUAL(BB_OK, bb_ring_registry_deregister(rb));
    TEST_ASSERT_EQUAL_UINT16(0, bb_ring_registry_count());
}

void test_bb_ring_registry_register_null_args_returns_invalid_arg(void)
{
    bb_ring_registry_test_reset();
    int dummy = 0;
    bb_ring_t fake = (bb_ring_t)(void *)&dummy;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ring_registry_register(NULL, fake));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ring_registry_register("x", NULL));
}

void test_bb_ring_registry_deregister_null_returns_invalid_arg(void)
{
    bb_ring_registry_test_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ring_registry_deregister(NULL));
}

void test_bb_ring_registry_deregister_unregistered_returns_not_found(void)
{
    bb_ring_registry_test_reset();
    int dummy = 0;
    bb_ring_t fake = (bb_ring_t)(void *)&dummy;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_ring_registry_deregister(fake));
}

// ---------------------------------------------------------------------------
// Overflow — best-effort, bounded loop well within BB_REGISTRY_SNAPSHOT_MAX (64)
// ---------------------------------------------------------------------------

void test_bb_ring_registry_overflow_returns_no_space(void)
{
    bb_ring_registry_test_reset();

    static int dummies[40];
    static char names[40][16];

    bb_err_t err = BB_OK;
    uint16_t registered = 0;
    for (int i = 0; i < 40; i++) {
        snprintf(names[i], sizeof names[i], "r%d", i);
        bb_ring_t fake = (bb_ring_t)(void *)&dummies[i];
        err = bb_ring_registry_register(names[i], fake);
        if (err != BB_OK) {
            break;
        }
        registered++;
    }

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_TRUE(registered > 0);
    TEST_ASSERT_EQUAL_UINT16(registered, bb_ring_registry_count());

    // A second overflowed attempt still fails cleanly (no state corruption).
    bb_ring_t another = (bb_ring_t)(void *)&dummies[39];
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_ring_registry_register("overflow2", another));
}

void test_bb_ring_registry_duplicate_name_returns_invalid_state(void)
{
    bb_ring_registry_test_reset();
    int a = 0, b = 0;
    bb_ring_t ra = (bb_ring_t)(void *)&a;
    bb_ring_t rb = (bb_ring_t)(void *)&b;

    TEST_ASSERT_EQUAL(BB_OK, bb_ring_registry_register("dup", ra));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_ring_registry_register("dup", rb));
    TEST_ASSERT_EQUAL_UINT16(1, bb_ring_registry_count());
}

// ---------------------------------------------------------------------------
// create auto-registers / destroy auto-deregisters
// ---------------------------------------------------------------------------

void test_bb_ring_registry_create_auto_registers(void)
{
    bb_ring_registry_test_reset();
    bb_ring_t r = make_ring("auto");
    TEST_ASSERT_EQUAL_UINT16(1, bb_ring_registry_count());
    bb_ring_destroy(r);
}

void test_bb_ring_registry_destroy_auto_deregisters(void)
{
    bb_ring_registry_test_reset();
    bb_ring_t r = make_ring("auto2");
    TEST_ASSERT_EQUAL_UINT16(1, bb_ring_registry_count());
    bb_ring_destroy(r);
    TEST_ASSERT_EQUAL_UINT16(0, bb_ring_registry_count());
}

// B1-419: a ring destroyed and recreated at runtime must re-register cleanly
// (not silently vanish from the diagnostic view after the first cycle).
void test_bb_ring_registry_destroy_then_recreate_reregisters(void)
{
    bb_ring_registry_test_reset();

    bb_ring_t r1 = make_ring("cyclic");
    TEST_ASSERT_EQUAL_UINT16(1, bb_ring_registry_count());
    bb_ring_destroy(r1);
    TEST_ASSERT_EQUAL_UINT16(0, bb_ring_registry_count());

    bb_ring_t r2 = make_ring("cyclic");
    TEST_ASSERT_EQUAL_UINT16(1, bb_ring_registry_count());
    bb_ring_destroy(r2);
    TEST_ASSERT_EQUAL_UINT16(0, bb_ring_registry_count());
}

void test_bb_ring_registry_destroy_null_noop(void)
{
    bb_ring_registry_test_reset();
    bb_ring_destroy(NULL);
    TEST_ASSERT_EQUAL_UINT16(0, bb_ring_registry_count());
}

// ---------------------------------------------------------------------------
// foreach — yields name + count/capacity/dropped/truncated/bytes_used
// ---------------------------------------------------------------------------

void test_bb_ring_registry_foreach_visits_all_with_fields(void)
{
    bb_ring_registry_test_reset();
    bb_ring_t r = make_ring("fe");

    // Push one entry so count/bytes_used are non-zero and observable via the
    // ring handle the callback receives.
    TEST_ASSERT_EQUAL(BB_OK, bb_ring_push(r, "x", 1, 1000, 7));

    foreach_capture_t cap = { 0 };
    bb_ring_registry_foreach(foreach_capture_cb, &cap);

    TEST_ASSERT_EQUAL(1, cap.calls);
    TEST_ASSERT_EQUAL_STRING("fe", cap.last_name);
    TEST_ASSERT_EQUAL_PTR(r, cap.last_r);
    TEST_ASSERT_EQUAL_size_t(1, bb_ring_count(cap.last_r));
    TEST_ASSERT_EQUAL_size_t(1, bb_ring_capacity(cap.last_r));
    TEST_ASSERT_EQUAL_size_t(1, bb_ring_bytes_used(cap.last_r));
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_dropped(cap.last_r));
    TEST_ASSERT_EQUAL_size_t(0, bb_ring_truncated(cap.last_r));

    bb_ring_destroy(r);
}

void test_bb_ring_registry_foreach_null_cb_is_noop(void)
{
    bb_ring_registry_test_reset();
    bb_ring_t r = make_ring("fe2");
    // Should not crash.
    bb_ring_registry_foreach(NULL, NULL);
    bb_ring_destroy(r);
}

void test_bb_ring_registry_foreach_empty_registry(void)
{
    bb_ring_registry_test_reset();
    foreach_capture_t cap = { 0 };
    bb_ring_registry_foreach(foreach_capture_cb, &cap);
    TEST_ASSERT_EQUAL(0, cap.calls);
}

// ---------------------------------------------------------------------------
// test_reset
// ---------------------------------------------------------------------------

void test_bb_ring_registry_test_reset_clears_all(void)
{
    bb_ring_registry_test_reset();
    bb_ring_t r = make_ring("reset");
    TEST_ASSERT_EQUAL_UINT16(1, bb_ring_registry_count());

    bb_ring_registry_test_reset();
    TEST_ASSERT_EQUAL_UINT16(0, bb_ring_registry_count());

    // The ring itself is still alive (test_reset only clears the registry,
    // not the ring); destroy it directly to avoid a leak. Its own
    // deregister-on-destroy call will be a harmless BB_ERR_NOT_FOUND no-op.
    bb_ring_destroy(r);
}

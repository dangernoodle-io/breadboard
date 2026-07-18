// Host tests for bb_data_http's pure core (B1-1033 PR-2, design KB 1443/
// 1444): attach table, fd-table client lifecycle, and the STATE dirty-mask
// sweep-step -- including the drain phase's clear-only-on-render-success
// invariant (a render_fn failure must leave the dirty bit set for a retry,
// never drop the update). No pthread/threads: every scenario below is a
// single-threaded, deterministic call sequence.

#include "unity.h"

#include "bb_data_http.h"
#include "bb_queue.h"

#include "../../components/bb_data_http/src/bb_data_http_internal.h"
#include "../../platform/host/bb_data_http/bb_data_http_host.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fixtures: a fake generation store + a fake render_fn built on top of it.
// ---------------------------------------------------------------------------

#define FAKE_GEN_MAX 8

typedef struct {
    const char *key;
    uint32_t    gen;
} fake_gen_entry_t;

static fake_gen_entry_t s_fake_gens[FAKE_GEN_MAX];
static size_t           s_fake_gen_count;

static void fake_gen_reset(void)
{
    s_fake_gen_count = 0;
    memset(s_fake_gens, 0, sizeof(s_fake_gens));
}

static void fake_gen_set(const char *key, uint32_t gen)
{
    for (size_t i = 0; i < s_fake_gen_count; i++) {
        if (strcmp(s_fake_gens[i].key, key) == 0) {
            s_fake_gens[i].gen = gen;
            return;
        }
    }
    s_fake_gens[s_fake_gen_count].key = key;
    s_fake_gens[s_fake_gen_count].gen = gen;
    s_fake_gen_count++;
}

static void fake_gen_bump(const char *key)
{
    for (size_t i = 0; i < s_fake_gen_count; i++) {
        if (strcmp(s_fake_gens[i].key, key) == 0) {
            s_fake_gens[i].gen++;
            return;
        }
    }
    fake_gen_set(key, 1);
}

static bb_err_t fake_generation_fn(const char *key, uint32_t *out_gen, void *ctx)
{
    (void)ctx;
    for (size_t i = 0; i < s_fake_gen_count; i++) {
        if (strcmp(s_fake_gens[i].key, key) == 0) {
            *out_gen = s_fake_gens[i].gen;
            return BB_OK;
        }
    }
    *out_gen = 0;
    return BB_OK;
}

static int  s_render_call_count;
static char s_last_rendered_key[64];

static bb_err_t fake_render_fn(const char *key, char *buf, size_t cap, size_t *out_len, void *ctx)
{
    (void)ctx;
    s_render_call_count++;
    strncpy(s_last_rendered_key, key, sizeof(s_last_rendered_key) - 1);
    s_last_rendered_key[sizeof(s_last_rendered_key) - 1] = '\0';

    uint32_t gen = 0;
    fake_generation_fn(key, &gen, NULL);

    int n = snprintf(buf, cap, "{\"key\":\"%s\",\"gen\":%" PRIu32 "}", key, gen);
    if (n < 0 || (size_t)n >= cap) return BB_ERR_NO_SPACE;  // LCOV_EXCL_LINE -- fixture buffers are always large enough
    *out_len = (size_t)n;
    return BB_OK;
}

static bb_err_t failing_render_fn(const char *key, char *buf, size_t cap, size_t *out_len, void *ctx)
{
    (void)key;
    (void)buf;
    (void)cap;
    (void)out_len;
    (void)ctx;
    return BB_ERR_INVALID_STATE;
}

// Renders a zero-length payload -- drives bb_data_http_host.c's host_send()
// through its len==0 skip-the-memcpy path (BB_ERR_INVALID_STATE is not
// involved; render_fn itself still succeeds).
static bb_err_t empty_render_fn(const char *key, char *buf, size_t cap, size_t *out_len, void *ctx)
{
    (void)key;
    (void)buf;
    (void)cap;
    (void)ctx;
    *out_len = 0;
    return BB_OK;
}

static void *failing_calloc(size_t n, size_t sz)
{
    (void)n;
    (void)sz;
    return NULL;
}

// Key that fake_generation_fn_with_failure() reports BB_ERR_INVALID_STATE
// for, instead of delegating to fake_generation_fn(); NULL (the reset_all()
// default) means "no key fails".
static const char *s_gen_fail_key;

static bb_err_t fake_generation_fn_with_failure(const char *key, uint32_t *out_gen, void *ctx)
{
    if (s_gen_fail_key && strcmp(key, s_gen_fail_key) == 0) return BB_ERR_INVALID_STATE;
    return fake_generation_fn(key, out_gen, ctx);
}

static void reset_all(void)
{
    bb_data_http_reset_for_test();
    bb_data_http_host_reset();
    fake_gen_reset();
    s_render_call_count = 0;
    s_last_rendered_key[0] = '\0';
    s_gen_fail_key = NULL;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void test_bb_data_http_init_idempotent(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_init(NULL));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_init(NULL));
}

void test_bb_data_http_init_max_clients_over_cap_returns_invalid_arg(void)
{
    reset_all();
    bb_data_http_cfg_t cfg = { .max_clients = 1000 };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_init(&cfg));
}

// A non-NULL cfg with max_clients == 0 falls back to the Kconfig default
// (see bb_data_http_cfg_t's doc comment: "0 -> CONFIG_BB_DATA_HTTP_MAX_
// CLIENTS"), the same as cfg == NULL -- exercises the `cfg && cfg->
// max_clients` ternary's cfg-non-NULL-but-zero case distinctly from both
// cfg == NULL and a genuinely nonzero cfg->max_clients.
void test_bb_data_http_init_cfg_non_null_zero_max_clients_uses_default(void)
{
    reset_all();
    bb_data_http_cfg_t cfg = { .max_clients = 0 };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_init(&cfg));

    // CONFIG_BB_DATA_HTTP_MAX_CLIENTS's C default is 2 (bb_data_http_common.c
    // Kconfig bridge) -- not exposed via the public header, so hardcoded
    // here rather than referencing the private macro from a test.
    bb_data_http_client_t *c1 = NULL, *c2 = NULL, *overflow = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&c1, 1, false));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&c2, 2, false));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_data_http_client_acquire(&overflow, 3, false));
}

// ---------------------------------------------------------------------------
// Attach table
// ---------------------------------------------------------------------------

void test_bb_data_http_attach_round_trip(void)
{
    reset_all();
    bb_data_http_init(NULL);

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_attach_count());
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_attach("k1", "topic.a"));
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_attach_count());

    // Re-attaching an already-attached key is idempotent: updates topic/kind
    // in place, does not grow the table.
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_attach_ex("k1", "topic.b", BB_DATA_HTTP_EVENT));
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_attach_count());
}

void test_bb_data_http_attach_null_or_empty_args_return_invalid_arg(void)
{
    reset_all();
    bb_data_http_init(NULL);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_attach(NULL, "t"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_attach("k", NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_attach("", "t"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_attach("k", ""));
}

void test_bb_data_http_attach_key_too_long_returns_invalid_arg(void)
{
    reset_all();
    bb_data_http_init(NULL);

    char key[BB_DATA_HTTP_KEY_MAX + 1];
    memset(key, 'k', sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_attach(key, "t"));
}

void test_bb_data_http_attach_topic_too_long_returns_invalid_arg(void)
{
    reset_all();
    bb_data_http_init(NULL);

    char topic[BB_DATA_HTTP_TOPIC_MAX + 1];
    memset(topic, 't', sizeof(topic) - 1);
    topic[sizeof(topic) - 1] = '\0';
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_attach("k", topic));
}

void test_bb_data_http_attach_capacity_full_returns_no_space(void)
{
    reset_all();
    bb_data_http_init(NULL);

    char keys[BB_DATA_HTTP_MAX_ATTACH + 1][16];
    for (int i = 0; i < BB_DATA_HTTP_MAX_ATTACH; i++) {
        snprintf(keys[i], sizeof(keys[i]), "k.%d", i);
        TEST_ASSERT_EQUAL(BB_OK, bb_data_http_attach(keys[i], "t"));
    }

    snprintf(keys[BB_DATA_HTTP_MAX_ATTACH], sizeof(keys[BB_DATA_HTTP_MAX_ATTACH]), "k.%d", BB_DATA_HTTP_MAX_ATTACH);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_data_http_attach(keys[BB_DATA_HTTP_MAX_ATTACH], "t"));

    // Re-attaching an already-attached key still succeeds even with the
    // table full.
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_attach(keys[0], "t2"));
}

// ---------------------------------------------------------------------------
// Client lifecycle (fd-table)
// ---------------------------------------------------------------------------

void test_bb_data_http_client_acquire_release_round_trip(void)
{
    reset_all();
    bb_data_http_init(NULL);

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_active_client_count());
    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&c, 1, false));
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_active_client_count());

    bb_data_http_client_release(c);
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_active_client_count());
}

void test_bb_data_http_client_release_null_is_noop(void)
{
    reset_all();
    bb_data_http_client_release(NULL);  // must not crash
}

void test_bb_data_http_client_acquire_before_init_returns_invalid_state(void)
{
    reset_all();
    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_data_http_client_acquire(&c, 1, false));
}

void test_bb_data_http_client_acquire_null_out_returns_invalid_arg(void)
{
    reset_all();
    bb_data_http_init(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_client_acquire(NULL, 1, false));
}

void test_bb_data_http_client_acquire_exhaustion_returns_no_space(void)
{
    reset_all();
    bb_data_http_cfg_t cfg = { .max_clients = 2 };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_init(&cfg));

    bb_data_http_client_t *c1 = NULL, *c2 = NULL, *c3 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&c1, 1, false));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&c2, 2, false));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_data_http_client_acquire(&c3, 3, false));
}

void test_bb_data_http_client_acquire_outbound_alloc_failure_returns_error(void)
{
    reset_all();
    bb_data_http_init(NULL);

    bb_queue_set_allocator(failing_calloc, free);
    bb_data_http_client_t *c = NULL;
    bb_err_t               rc = bb_data_http_client_acquire(&c, 1, false);
    bb_queue_reset_allocator();

    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);
}

// ---------------------------------------------------------------------------
// topic_filter matching -- NULL/"" == all attached keys; otherwise exact
// match only. Both proven via the fresh-render-on-connect dirty bits an
// acquire sets, before any sweep_step() runs.
// ---------------------------------------------------------------------------

void test_bb_data_http_client_topic_filter_null_subscribes_all(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_attach("k1", "topic.a");
    bb_data_http_attach("k2", "topic.b");

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire_ex(&c, 5, NULL, false));

    TEST_ASSERT_EQUAL_UINT32(0x3u, bb_data_http_client_dirty_mask_for_test(c));
}

void test_bb_data_http_client_topic_filter_empty_string_subscribes_all(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_attach("k1", "topic.a");
    bb_data_http_attach("k2", "topic.b");

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire_ex(&c, 5, "", false));

    TEST_ASSERT_EQUAL_UINT32(0x3u, bb_data_http_client_dirty_mask_for_test(c));
}

void test_bb_data_http_client_topic_filter_exact_match_only(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_attach("k1", "topic.a");
    bb_data_http_attach("k2", "topic.b");

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire_ex(&c, 5, "topic.a", false));

    TEST_ASSERT_EQUAL_UINT32(0x1u, bb_data_http_client_dirty_mask_for_test(c));  // only k1 (attach idx 0)
}

void test_bb_data_http_client_topic_filter_no_match_subscribes_nothing(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_attach("k1", "topic.a");

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire_ex(&c, 5, "topic.nope", false));

    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dirty_mask_for_test(c));
}

// An over-length topic_filter must be REJECTED (BB_ERR_INVALID_ARG), the
// same as bb_data_http_attach_ex() rejects an over-length topic -- never
// silently truncated by bb_strlcpy into a filter that mis-subscribes the
// client to a topic name it never asked for.
void test_bb_data_http_client_acquire_ex_topic_filter_too_long_returns_invalid_arg(void)
{
    reset_all();
    bb_data_http_init(NULL);

    char filter[BB_DATA_HTTP_TOPIC_MAX + 1];
    memset(filter, 't', sizeof(filter) - 1);
    filter[sizeof(filter) - 1] = '\0';

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_client_acquire_ex(&c, 5, filter, false));
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_active_client_count());  // no slot consumed
}

// ---------------------------------------------------------------------------
// STATE dirty-mask sweep-step: detect, coalesce, render, drain.
// ---------------------------------------------------------------------------

void test_bb_data_http_sweep_step_renders_dirty_state_key(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();

    bb_data_http_attach("k1", "topic.a");
    fake_gen_set("k1", 5);

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&c, 42, false));

    bb_data_http_sweep_step();

    TEST_ASSERT_TRUE(s_render_call_count >= 1);
    TEST_ASSERT_EQUAL_STRING("k1", s_last_rendered_key);
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dirty_mask_for_test(c));
    TEST_ASSERT_EQUAL_UINT32(5u, bb_data_http_client_seen_gen_for_test(c, 0));

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
    int    fd     = -1;
    bool   is_ws  = true;
    char   buf[128];
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_at(0, &fd, &is_ws, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_INT(42, fd);
    TEST_ASSERT_FALSE(is_ws);
    buf[len] = '\0';
    TEST_ASSERT_EQUAL_STRING("{\"key\":\"k1\",\"gen\":5}", buf);
}

void test_bb_data_http_sweep_step_coalesces_multiple_bumps_into_one_render(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach("k1", "topic.a");

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);
    bb_data_http_sweep_step();  // drains the fresh-render-on-connect dirty bit
    bb_data_http_host_reset();
    s_render_call_count = 0;

    fake_gen_bump("k1");
    fake_gen_bump("k1");
    fake_gen_bump("k1");

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_INT(1, s_render_call_count);  // coalesced into ONE render
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
}

void test_bb_data_http_sweep_step_multiple_dirty_keys_all_rendered(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach("k1", "topic.a");
    bb_data_http_attach("k2", "topic.b");

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(2, bb_data_http_host_frame_count());
}

void test_bb_data_http_sweep_step_skips_event_kind_keys(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach_ex("ev1", "topic.a", BB_DATA_HTTP_EVENT);

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);

    // Fresh-render-on-connect only marks STATE keys dirty.
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dirty_mask_for_test(c));

    fake_gen_bump("ev1");
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_host_frame_count());
}

void test_bb_data_http_sweep_step_without_render_fn_still_clears_dirty(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    // no render_fn installed
    bb_data_http_attach("k1", "topic.a");

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dirty_mask_for_test(c));
}

void test_bb_data_http_sweep_step_without_generation_fn_still_drains_connect_dirty(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    // no generation_fn installed
    bb_data_http_attach("k1", "topic.a");

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
}

void test_bb_data_http_sweep_step_render_failure_skips_frame_without_crash(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(failing_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach("k1", "topic.a");

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);

    size_t fails_before = bb_data_http_render_fail_count();
    bb_data_http_sweep_step();
    // Fail a second consecutive sweep too, so the rate-limit modulo check
    // (fail count != 1) is actually exercised, not just the first-failure
    // branch.
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_host_frame_count());
    // Bit stays SET on render failure (clear-only-on-success) -- the key
    // must be retried next sweep, not silently dropped.
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_dirty_mask_for_test(c));
    TEST_ASSERT_EQUAL_UINT(fails_before + 2, bb_data_http_render_fail_count());
}

void test_bb_data_http_sweep_step_push_without_send_fn_leaves_outbound_queued(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    // no send_fn installed
    bb_data_http_attach("k1", "topic.a");

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_client_outbound_count_for_test(c));
}

// ---------------------------------------------------------------------------
// THE render-failure-retry test (KB 1443, the drain-phase's genuinely
// load-bearing invariant): the dirty bit is cleared ONLY on render success,
// never unconditionally. This is DISCRIMINATING by construction: if the
// implementation instead cleared the bit before/regardless of calling
// render_fn (the old, disproven "clear-before-render" claim), the first
// sweep_step() below would clear the bit on the failing render, the retry
// sweep would find nothing dirty, render_fn would never be called a second
// time, and the final frame/seen-gen assertions would fail. (Cross-sweep
// generation-bump safety is a separate, still-true property: the detect
// phase records state_seen_gen from the pre-render generation and runs to
// completion before any render call -- see bb_data_http.h's HARD INVARIANT
// comment and bb_data_http_sweep_step()'s doc.)
// ---------------------------------------------------------------------------

void test_bb_data_http_sweep_step_render_failure_retries_on_next_sweep(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach("k1", "topic.a");
    fake_gen_set("k1", 5);

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);  // fresh-render-on-connect sets the dirty bit

    size_t fails_before = bb_data_http_render_fail_count();

    // First sweep: render_fn fails. If the bit were cleared unconditionally,
    // this would be the update's only chance to ever be sent.
    bb_data_http_set_render_fn(failing_render_fn, NULL);
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_host_frame_count());
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_dirty_mask_for_test(c));  // still dirty
    TEST_ASSERT_EQUAL_UINT(fails_before + 1, bb_data_http_render_fail_count());

    // No generation change, no new touch -- the ONLY reason the key is
    // re-attempted is that the bit was never cleared.
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());  // retried and sent -- NOT lost
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dirty_mask_for_test(c));  // cleared on success
    TEST_ASSERT_EQUAL_UINT32(5u, bb_data_http_client_seen_gen_for_test(c, 0));
}

// ---------------------------------------------------------------------------
// Detect-phase generation_fn failure (bb_data_http_sweep_step()'s detect
// loop): a failing generation_fn must be skipped entirely for that key --
// no dirty bit, no state_seen_gen update, no render call -- rather than
// treating the failure as "unchanged" or crashing on an uninitialized `gen`.
// ---------------------------------------------------------------------------

void test_bb_data_http_sweep_step_generation_fn_failure_skips_key(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_set_generation_fn(fake_generation_fn_with_failure, NULL);
    bb_data_http_attach("k1", "topic.a");
    fake_gen_set("k1", 5);

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);
    bb_data_http_sweep_step();  // drains the fresh-render-on-connect dirty bit; seen_gen -> 5
    bb_data_http_host_reset();
    s_render_call_count = 0;

    // generation_fn now fails for k1: the detect phase's
    // `s_gen_fn(...) != BB_OK` branch must `continue` past it entirely.
    s_gen_fail_key = "k1";
    fake_gen_bump("k1");
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_INT(0, s_render_call_count);
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_host_frame_count());
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dirty_mask_for_test(c));
    TEST_ASSERT_EQUAL_UINT32(5u, bb_data_http_client_seen_gen_for_test(c, 0));  // unchanged, not bumped to 6
}

// ---------------------------------------------------------------------------
// Detect-phase per-client subscription filter: a client not subscribed to a
// dirtied key's topic must be skipped by client_subscribes() without its
// dirty mask / seen_gen being touched, while a subscribed client still
// re-renders normally.
// ---------------------------------------------------------------------------

void test_bb_data_http_sweep_step_detect_phase_skips_unsubscribed_client(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach("k1", "topic.a");
    fake_gen_set("k1", 1);

    bb_data_http_client_t *sub = NULL, *unsub = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire_ex(&sub, 1, "topic.a", false));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire_ex(&unsub, 2, "topic.b", false));

    bb_data_http_sweep_step();  // drains `sub`'s fresh-connect dirty bit; `unsub` was never dirty
    bb_data_http_host_reset();
    s_render_call_count = 0;

    fake_gen_bump("k1");
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_INT(1, s_render_call_count);  // only `sub` re-renders
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dirty_mask_for_test(unsub));
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_seen_gen_for_test(unsub, 0));
}

// ---------------------------------------------------------------------------
// Drain-phase per-key dirty-bit skip: with multiple attached keys but only
// one dirty for this client, the drain loop's other attach-index iterations
// must `continue` past the not-set bit rather than rendering it.
// ---------------------------------------------------------------------------

void test_bb_data_http_sweep_step_drain_phase_skips_non_dirty_key(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach("k1", "topic.a");
    bb_data_http_attach("k2", "topic.b");

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire_ex(&c, 1, "topic.a", false));
    TEST_ASSERT_EQUAL_UINT32(0x1u, bb_data_http_client_dirty_mask_for_test(c));  // only k1 (idx 0)

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_INT(1, s_render_call_count);
    TEST_ASSERT_EQUAL_STRING("k1", s_last_rendered_key);
}

// ---------------------------------------------------------------------------
// Render-failure log rate-limit cadence: drives BB_DATA_HTTP_RENDER_FAIL_LOG_
// EVERY (32) consecutive failures on one key so the rate-limit condition's
// `fail_count % 32 == 0` operand is evaluated both true (count==32) and
// false (every other count) within one deterministic run, not just the
// short-circuited count==1 case the existing failure tests already cover.
// ---------------------------------------------------------------------------

void test_bb_data_http_sweep_step_render_failure_log_rate_limit_both_outcomes(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(failing_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach("k1", "topic.a");

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);

    for (int i = 0; i < 32; i++) {
        bb_data_http_sweep_step();
    }

    TEST_ASSERT_EQUAL_UINT(32, bb_data_http_render_fail_count());
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_dirty_mask_for_test(c));  // still retrying
}

// ---------------------------------------------------------------------------
// Test-hook defensive guards: every _for_test() getter tolerates a NULL
// client (returns its zero value) and bb_data_http_client_seen_gen_for_test()
// additionally bounds-checks idx.
// ---------------------------------------------------------------------------

void test_bb_data_http_for_test_helpers_defend_against_null_and_out_of_range(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dirty_mask_for_test(NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_seen_gen_for_test(NULL, 0));
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_client_outbound_count_for_test(NULL));

    bb_data_http_init(NULL);
    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_seen_gen_for_test(c, BB_DATA_HTTP_MAX_ATTACH));
}

// ---------------------------------------------------------------------------
// Host capture-stub backend (platform/host/bb_data_http/bb_data_http_host.c)
// edge cases: capture-ring capacity, zero-length frames, out-of-range
// index, and optional (NULL) output parameters.
// ---------------------------------------------------------------------------

void test_bb_data_http_host_frame_capture_stops_at_capacity(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach("k1", "topic.a");

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);

    // BB_DATA_HTTP_HOST_CAPTURE_MAX (32) frames fill the ring; every send
    // past that must be silently dropped (host_send() returns
    // BB_ERR_NO_SPACE, which sweep_step()'s send loop ignores by design)
    // rather than overflowing s_frames[].
    for (int i = 0; i < 40; i++) {
        fake_gen_bump("k1");
        bb_data_http_sweep_step();
    }

    TEST_ASSERT_EQUAL_UINT(32, bb_data_http_host_frame_count());
}

void test_bb_data_http_host_send_zero_length_frame_skips_copy(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(empty_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach("k1", "topic.a");
    fake_gen_set("k1", 1);

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
    int    fd    = -1;
    bool   is_ws = true;
    char   buf[8];
    size_t len   = 99;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_at(0, &fd, &is_ws, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_UINT(0, len);
}

void test_bb_data_http_host_frame_at_out_of_range_returns_not_found(void)
{
    reset_all();
    bb_data_http_host_install_send();

    int    fd    = -1;
    bool   is_ws = false;
    char   buf[8];
    size_t len   = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_data_http_host_frame_at(0, &fd, &is_ws, buf, sizeof(buf), &len));
}

void test_bb_data_http_host_frame_at_null_out_params_are_optional(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach("k1", "topic.a");
    fake_gen_set("k1", 1);

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
    // Every output pointer may be NULL to skip that field (see
    // bb_data_http_host.h) -- must not crash / must still succeed.
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_at(0, NULL, NULL, NULL, 0, NULL));

    // A non-NULL buf with buf_cap == 0 must also skip the copy (the `buf &&
    // buf_cap > 0` guard's other half) rather than writing past a
    // zero-capacity buffer.
    char zero_cap_buf[1];
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_at(0, NULL, NULL, zero_cap_buf, 0, NULL));
}

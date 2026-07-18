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

// Mirrors max_clients's own shrink-only-override bound: event_ring_capacity
// may only shrink CONFIG_BB_DATA_HTTP_EVENT_RING_CAPACITY at runtime, never
// grow it.
void test_bb_data_http_init_event_ring_capacity_over_cap_returns_invalid_arg(void)
{
    reset_all();
    bb_data_http_cfg_t cfg = { .event_ring_capacity = 1000 };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_init(&cfg));
}

// A shared-EVENT-ring allocation failure at init time must propagate as an
// error rather than leaving bb_data_http half-initialized.
void test_bb_data_http_init_event_ring_alloc_failure_returns_error(void)
{
    reset_all();
    bb_queue_set_allocator(failing_calloc, free);
    bb_err_t rc = bb_data_http_init(NULL);
    bb_queue_reset_allocator();

    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);
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

// EVENT-kind keys no longer skip the sweep entirely (B1-1033 PR-3): a
// generation bump on an EVENT key pushes into the shared ring and a
// subscribed client drains it on the SAME sweep_step() call, distinctly
// from STATE's dirty-mask path (fresh-render-on-connect still never marks
// an EVENT key dirty -- that mechanism is STATE-only).
void test_bb_data_http_sweep_step_event_kind_key_delivers_via_shared_ring(void)
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

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_event_cursor_for_test(c));
}

// ---------------------------------------------------------------------------
// EVENT push path (B1-1033 PR-3, design KB 1443/1444): shared ring + per-
// client cursor + topic_filter + backpressure/dropped:N. No pthread/threads
// -- every scenario below is a single-threaded, deterministic call sequence.
// ---------------------------------------------------------------------------

// A no-render_fn EVENT bump must still advance the module's own last-seen
// generation for that key (degrade-gracefully, mirrors STATE's identical
// no-render_fn drain path) -- otherwise a later render_fn install would
// spuriously re-fire on a generation this sweep already saw.
void test_bb_data_http_sweep_step_event_kind_key_without_render_fn_advances_gen(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    // no render_fn installed
    bb_data_http_attach_ex("ev1", "topic.a", BB_DATA_HTTP_EVENT);
    fake_gen_set("ev1", 1);

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);

    bb_data_http_sweep_step();
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_host_frame_count());

    // Now install a render_fn and re-sweep with NO further generation bump:
    // if the module-level last-seen generation had not advanced above, this
    // would spuriously fire now.
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_host_frame_count());
}

// A failing render_fn on an EVENT key must be retried next sweep (the
// module's last-seen generation is left unchanged on failure), exactly
// mirroring STATE's clear-only-on-success invariant -- never silently
// skipping the event.
void test_bb_data_http_sweep_step_event_render_failure_retries_on_next_sweep(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach_ex("ev1", "topic.a", BB_DATA_HTTP_EVENT);
    fake_gen_set("ev1", 1);

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);

    size_t fails_before = bb_data_http_render_fail_count();

    bb_data_http_set_render_fn(failing_render_fn, NULL);
    bb_data_http_sweep_step();
    // A second consecutive failure too, so the rate-limit modulo check's
    // `fail_count != 1` operand is actually exercised (mirrors the STATE
    // drain path's identical rate-limit coverage rationale).
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_host_frame_count());
    TEST_ASSERT_EQUAL_UINT(fails_before + 2, bb_data_http_render_fail_count());

    // No further generation change -- the ONLY reason this is re-attempted
    // is that the module-level last-seen generation was never advanced.
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());  // retried and sent -- NOT lost
}

// Cursor advance: a client's event_cursor tracks the next undrained global
// sequence number, advancing by exactly one per drained ring entry across
// successive sweeps.
void test_bb_data_http_event_cursor_advances_across_sweeps(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach_ex("ev1", "topic.a", BB_DATA_HTTP_EVENT);

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&c, 1, false);
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_event_cursor_for_test(c));

    fake_gen_bump("ev1");
    bb_data_http_sweep_step();
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_event_cursor_for_test(c));

    fake_gen_bump("ev1");
    bb_data_http_sweep_step();
    TEST_ASSERT_EQUAL_UINT32(2u, bb_data_http_client_event_cursor_for_test(c));

    // A sweep with no generation change must not advance the cursor further
    // -- there is nothing new in the ring to drain.
    bb_data_http_sweep_step();
    TEST_ASSERT_EQUAL_UINT32(2u, bb_data_http_client_event_cursor_for_test(c));

    TEST_ASSERT_EQUAL_UINT(2, bb_data_http_host_frame_count());
}

// Multi-client independent cursors: a client that acquires AFTER an event
// has already been pushed starts at the ring's CURRENT head (no backlog
// replay), while an earlier client keeps draining from where it left off --
// each client's own event_cursor advances independently.
void test_bb_data_http_event_multiple_clients_independent_cursors(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach_ex("ev1", "topic.a", BB_DATA_HTTP_EVENT);

    bb_data_http_client_t *a = NULL, *b = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&a, 1, false));

    fake_gen_bump("ev1");
    bb_data_http_sweep_step();  // delivered to `a` only -- `b` doesn't exist yet
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_event_cursor_for_test(a));
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());

    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&b, 2, false));
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_event_cursor_for_test(b));  // no backlog

    fake_gen_bump("ev1");
    bb_data_http_sweep_step();  // delivered to BOTH `a` and `b`

    TEST_ASSERT_EQUAL_UINT32(2u, bb_data_http_client_event_cursor_for_test(a));
    TEST_ASSERT_EQUAL_UINT32(2u, bb_data_http_client_event_cursor_for_test(b));
    TEST_ASSERT_EQUAL_UINT(3, bb_data_http_host_frame_count());  // 1 (a only) + 2 (a and b)

    int fd_a1 = -1, fd_a2 = -1, fd_b1 = -1;
    bb_data_http_host_frame_at(0, &fd_a1, NULL, NULL, 0, NULL);
    bb_data_http_host_frame_at(1, &fd_a2, NULL, NULL, 0, NULL);
    bb_data_http_host_frame_at(2, &fd_b1, NULL, NULL, 0, NULL);
    TEST_ASSERT_EQUAL_INT(1, fd_a1);
    TEST_ASSERT_TRUE(fd_a2 == 1 || fd_a2 == 2);
    TEST_ASSERT_TRUE(fd_b1 == 1 || fd_b1 == 2);
    TEST_ASSERT_NOT_EQUAL(fd_a2, fd_b1);  // second event fanned out to BOTH fds
}

// topic_filter on events: a client filtered to a topic other than the one an
// EVENT key is attached under must NOT receive it, while still correctly
// receiving an event under its own subscribed topic.
void test_bb_data_http_event_topic_filter_excludes_non_matching_topic(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach_ex("ev1", "topic.a", BB_DATA_HTTP_EVENT);
    bb_data_http_attach_ex("ev2", "topic.b", BB_DATA_HTTP_EVENT);

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire_ex(&c, 1, "topic.a", false));

    fake_gen_bump("ev2");  // topic.b -- not subscribed
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_host_frame_count());
    // Cursor still advances past the filtered-out entry -- it was examined
    // and correctly skipped, not left stuck.
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_event_cursor_for_test(c));
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dropped_count(c));  // filtered != dropped

    fake_gen_bump("ev1");  // topic.a -- subscribed
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
    TEST_ASSERT_EQUAL_UINT32(2u, bb_data_http_client_event_cursor_for_test(c));
    int fd = -1;
    char buf[128];
    size_t len = 0;
    bb_data_http_host_frame_at(0, &fd, NULL, buf, sizeof(buf), &len);
    buf[len] = '\0';
    TEST_ASSERT_EQUAL_STRING("{\"key\":\"ev1\",\"gen\":1}", buf);
}

// Ring wrap: when more EVENT entries are pushed within a single sweep_step()
// than the shared ring's own capacity holds, a client whose cursor had not
// yet drained the evicted entries counts the whole gap as dropped, fast-
// forwards past it, and gets a "dropped:N" marker frame queued ahead of the
// next entry it CAN still see.
void test_bb_data_http_event_ring_wrap_drops_evicted_gap_with_marker(void)
{
    reset_all();
    bb_data_http_cfg_t cfg = { .event_ring_capacity = 2 };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_init(&cfg));
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach_ex("ev0", "t", BB_DATA_HTTP_EVENT);
    bb_data_http_attach_ex("ev1", "t", BB_DATA_HTTP_EVENT);
    bb_data_http_attach_ex("ev2", "t", BB_DATA_HTTP_EVENT);
    bb_data_http_attach_ex("ev3", "t", BB_DATA_HTTP_EVENT);

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&c, 1, false));
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_event_cursor_for_test(c));

    // All four keys bump BEFORE this single sweep_step() call -- the ring
    // (capacity 2) can only hold the last two of the four pushes this one
    // detect phase produces, evicting the first two before the client's
    // drain (which only runs once, at the end of this same call) ever sees
    // them.
    fake_gen_bump("ev0");
    fake_gen_bump("ev1");
    fake_gen_bump("ev2");
    fake_gen_bump("ev3");
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT32(2u, bb_data_http_client_dropped_count(c));
    TEST_ASSERT_EQUAL_UINT32(4u, bb_data_http_client_event_cursor_for_test(c));  // caught up to head

    // 3 frames: the "dropped:2" marker, then ev2, then ev3 (the two ring
    // entries that survived the eviction).
    TEST_ASSERT_EQUAL_UINT(3, bb_data_http_host_frame_count());
    char   buf[128];
    size_t len = 0;
    bb_data_http_host_frame_at(0, NULL, NULL, buf, sizeof(buf), &len);
    buf[len] = '\0';
    TEST_ASSERT_EQUAL_STRING("{\"dropped\":2}", buf);
    bb_data_http_host_frame_at(1, NULL, NULL, buf, sizeof(buf), &len);
    buf[len] = '\0';
    TEST_ASSERT_EQUAL_STRING("{\"key\":\"ev2\",\"gen\":1}", buf);
    bb_data_http_host_frame_at(2, NULL, NULL, buf, sizeof(buf), &len);
    buf[len] = '\0';
    TEST_ASSERT_EQUAL_STRING("{\"key\":\"ev3\",\"gen\":1}", buf);
}

// Backpressure: a client's own outbound queue filling up (independent of
// the shared ring's own capacity, which stays generous here) must drop
// EVENTs for that client only, never block/evict for anyone else, and
// surface a "dropped:N" marker as soon as room frees up.
void test_bb_data_http_event_client_outbound_full_drops_with_marker(void)
{
    reset_all();
    bb_data_http_init(NULL);  // event_ring_capacity default (16) -- generous
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    // no send_fn yet -- outbound is never drained, so it fills up
    bb_data_http_attach_ex("ev1", "t", BB_DATA_HTTP_EVENT);

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&c, 1, false));

    // CONFIG_BB_DATA_HTTP_OUTBOUND_CAPACITY's C default is 8 (bb_data_http_
    // common.c's Kconfig bridge) -- not exposed via the public header, so
    // hardcoded here rather than referencing the private macro from a test
    // (mirrors test_bb_data_http_init_cfg_non_null_zero_max_clients_uses_
    // default's identical rationale for CONFIG_BB_DATA_HTTP_MAX_CLIENTS).
    for (int i = 0; i < 8; i++) {
        fake_gen_bump("ev1");
        bb_data_http_sweep_step();
    }
    TEST_ASSERT_EQUAL_UINT(8, bb_data_http_client_outbound_count_for_test(c));
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dropped_count(c));

    // The 9th event finds outbound full -- dropped for `c`, NOT evicted from
    // the shared ring (a 2nd client would still see every one of these).
    fake_gen_bump("ev1");
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_dropped_count(c));
    TEST_ASSERT_EQUAL_UINT(8, bb_data_http_client_outbound_count_for_test(c));  // unchanged -- still full

    // The 10th event: outbound is STILL full, so even the "dropped:1" marker
    // itself has no room to be queued yet -- it must stay pending (neither
    // the marker nor this event gets queued) rather than ever being force-
    // pushed past the outbound's own room check.
    fake_gen_bump("ev1");
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT32(2u, bb_data_http_client_dropped_count(c));
    TEST_ASSERT_EQUAL_UINT(8, bb_data_http_client_outbound_count_for_test(c));  // still unchanged

    // Now drain the backlog via send_fn and push one more event: the queued
    // "dropped:2" marker must be delivered ahead of the fresh event, as soon
    // as outbound has room again.
    bb_data_http_host_install_send();
    bb_data_http_sweep_step();  // no new bump -- just flushes the 8 backlog frames
    TEST_ASSERT_EQUAL_UINT(8, bb_data_http_host_frame_count());
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_client_outbound_count_for_test(c));

    fake_gen_bump("ev1");
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(10, bb_data_http_host_frame_count());  // +marker, +event
    char   buf[64];
    size_t len = 0;
    bb_data_http_host_frame_at(8, NULL, NULL, buf, sizeof(buf), &len);
    buf[len] = '\0';
    TEST_ASSERT_EQUAL_STRING("{\"dropped\":2}", buf);
}

// A pending "dropped:N" marker must reach the wire even when the EVENT feed
// goes quiet afterward -- drain_client_events() attempts the flush every
// sweep, not only when a new ring entry happens to piggyback the check (see
// bb_data_http_sweep_step()'s EVENT drain doc). Client outbound fills to
// capacity (dropping the next event and leaving a marker pending), then a
// sweep with NO further generation bump -- nothing new in the ring -- must
// still deliver the marker as soon as outbound has room.
void test_bb_data_http_event_pending_marker_flushes_on_quiet_sweep(void)
{
    reset_all();
    bb_data_http_init(NULL);  // event_ring_capacity default (16) -- generous
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    // no send_fn yet -- outbound is never drained, so it fills up
    bb_data_http_attach_ex("ev1", "t", BB_DATA_HTTP_EVENT);

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&c, 1, false));

    // Saturate outbound (capacity 8), then drop one more to leave a marker
    // pending (mirrors test_bb_data_http_event_client_outbound_full_drops_
    // with_marker's own setup).
    for (int i = 0; i < 8; i++) {
        fake_gen_bump("ev1");
        bb_data_http_sweep_step();
    }
    fake_gen_bump("ev1");
    bb_data_http_sweep_step();
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_dropped_count(c));

    // Drain the backlog via send_fn -- outbound now has room -- but issue NO
    // further generation bump before the next sweep: the ring has nothing
    // new for this client to drain, yet the pending marker must still flush.
    bb_data_http_host_install_send();
    bb_data_http_sweep_step();  // flushes the 8 backlog frames
    TEST_ASSERT_EQUAL_UINT(8, bb_data_http_host_frame_count());
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_client_outbound_count_for_test(c));

    bb_data_http_sweep_step();  // no new event -- must still flush the marker

    TEST_ASSERT_EQUAL_UINT(9, bb_data_http_host_frame_count());
    char   buf[64];
    size_t len = 0;
    bb_data_http_host_frame_at(8, NULL, NULL, buf, sizeof(buf), &len);
    buf[len] = '\0';
    TEST_ASSERT_EQUAL_STRING("{\"dropped\":1}", buf);
}

// A client whose OWN outbound is already saturated by unrelated STATE
// traffic must drop an EVENT for itself only -- a second client subscribed
// only to the EVENT topic (never touched by that STATE traffic) must
// receive the same event with zero drops, proving the shared ring/drain
// never lets one client's backpressure affect another's.
void test_bb_data_http_event_slow_client_does_not_affect_other_clients(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    // no send_fn -- nothing is ever flushed, so outbound only ever grows.
    bb_data_http_attach("s1", "state.only");
    bb_data_http_attach_ex("ev1", "event.only", BB_DATA_HTTP_EVENT);

    bb_data_http_client_t *slow = NULL, *fast = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire_ex(&slow, 1, NULL, false));            // all topics
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire_ex(&fast, 2, "event.only", false));    // events only

    // Saturate `slow`'s outbound (capacity 8) with 8 STATE renders --
    // `fast` is not subscribed to "state.only" so none of this reaches it.
    // (`slow`'s connect-time fresh-render-on-connect dirty bit covers the
    // first of these 8 sweeps.)
    for (int i = 0; i < 8; i++) {
        bb_data_http_sweep_step();
        if (i < 7) fake_gen_bump("s1");  // re-dirty for the next iteration
    }
    TEST_ASSERT_EQUAL_UINT(8, bb_data_http_client_outbound_count_for_test(slow));
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_client_outbound_count_for_test(fast));

    // Now push one EVENT both clients are subscribed to.
    fake_gen_bump("ev1");
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_dropped_count(slow));   // dropped for `slow` only
    TEST_ASSERT_EQUAL_UINT(8, bb_data_http_client_outbound_count_for_test(slow));  // unchanged -- still full

    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dropped_count(fast));   // untouched
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_client_outbound_count_for_test(fast));  // delivered normally
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
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dropped_count(NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_event_cursor_for_test(NULL));

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

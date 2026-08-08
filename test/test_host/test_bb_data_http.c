// Host tests for bb_data_http's pure core (B1-1033 PR-2, design KB 1443/
// 1444): attach table, fd-table client lifecycle, and the STATE dirty-mask
// sweep-step -- including the drain phase's clear-only-on-render-success
// invariant (a render_fn failure must leave the dirty bit set for a retry,
// never drop the update). No pthread/threads: every scenario below is a
// single-threaded, deterministic call sequence.

#include "unity.h"

#include "bb_data_http.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_queue.h"

#include "../../components/bb_data_http/src/bb_data_http_internal.h"
#include "../../platform/host/bb_data_http/bb_data_http_host.h"
#include "test_openapi_capture.h"

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

// B1-1424 HIGH fix: simulates a foreign task's disconnect callback racing
// IN THE MIDDLE of a sweep_step() call already in progress for this same
// client -- the exact hazard bb_data_http_client_request_release() closes
// (see its doc, bb_data_http.h). Real hardware can't be forced into that
// window from a host test (no threads here), so this fakes the same
// SEQUENCING instead: a render_fn that calls
// bb_data_http_client_request_release() on a client captured via `ctx`
// partway through the CURRENT sweep_step() call's own drain-phase work for
// that client, then still returns a valid rendered value -- exactly as if
// the request had raced in from another task moments earlier and this
// task's in-flight render call simply hadn't observed it yet at the top of
// the loop.
static bb_err_t mid_sweep_release_render_fn(const char *key, char *buf, size_t cap,
                                            size_t *out_len, void *ctx)
{
    bb_data_http_client_request_release((bb_data_http_client_t *)ctx);
    return fake_render_fn(key, buf, cap, out_len, NULL);
}

// B1-1450: simulates a second, concurrent bb_data_http_push_pump() caller
// claiming a NEWER generation for the SAME key while THIS caller's own
// render_fn call is still in flight -- same same-call-sequencing trick as
// mid_sweep_release_render_fn() above, applied to the claim/revoke
// protocol's compare-and-restore race instead of the client-release race.
// Directly claims gen+1 via the test-only setter (bypassing the real
// claim/commit/revoke protocol, standing in for a second task's own claim
// step), then fails so the ORIGINAL caller's own revoke attempt (for the
// generation it actually claimed) runs against an already-moved-on value.
static bb_err_t concurrent_claim_then_fail_render_fn(const char *key, char *buf, size_t cap,
                                                      size_t *out_len, void *ctx)
{
    (void)buf;
    (void)cap;
    (void)out_len;
    (void)ctx;
    uint32_t gen = 0;
    fake_generation_fn(key, &gen, NULL);
    bb_data_http_event_last_gen_set_for_test(key, gen + 1);
    return BB_ERR_INVALID_STATE;
}

// B1-1450 round 2: simulates a second, concurrent bb_data_http_push_pump()
// caller claiming AND COMMITTING a NEWER generation for the SAME key while
// THIS caller's own render_fn call is still in flight, then SUCCEEDING --
// unlike concurrent_claim_then_fail_render_fn() above (which drives the
// REVOKE compare-and-restore's "leave it alone" branch), this drives
// COMMIT's own compare-and-discard: a stale-but-successful render must be
// discarded once a newer claim already exists, per push_pump()'s own doc
// (bb_data_http.h). Same same-call-sequencing trick, applied to the
// remaining round-1 gap: locking only the CLAIM step (not COMMIT) let a
// slow claimant's stale-but-successful render still land a duplicate push.
static bb_err_t concurrent_claim_then_succeed_render_fn(const char *key, char *buf, size_t cap,
                                                         size_t *out_len, void *ctx)
{
    uint32_t gen = 0;
    fake_generation_fn(key, &gen, NULL);
    bb_data_http_event_last_gen_set_for_test(key, gen + 1);
    return fake_render_fn(key, buf, cap, out_len, ctx);
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
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c1));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c2));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &overflow));
}

// ---------------------------------------------------------------------------
// Attach table
// ---------------------------------------------------------------------------

void test_bb_data_http_attach_round_trip(void)
{
    reset_all();
    bb_data_http_init(NULL);

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_attach_count());
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"}));
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_attach_count());

    // Re-attaching an already-attached key is idempotent: updates topic/kind
    // in place, does not grow the table.
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.b",.kind=BB_DATA_HTTP_EVENT}));
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_attach_count());
}

// B1-1439: a NULL cfg pointer is not "use defaults" -- it's a caller bug,
// consistent with every other bb_*_cfg_t entry point in this repo (mirrors
// bb_wdt_task_subscribe(NULL)'s own BB_ERR_INVALID_ARG contract, B1-1460).
void test_bb_data_http_attach_null_cfg_returns_invalid_arg(void)
{
    reset_all();
    bb_data_http_init(NULL);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_attach(NULL));
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_attach_count());
}

// B1-1439 zero-default pin: an omitted cfg->kind field (zero-init ->
// BB_DATA_HTTP_STATE, since the enum's explicit `= 0`) must reproduce the
// pre-collapse no-kind-arg bb_data_http_attach(key, topic) call exactly --
// same return, and the attached key must actually behave as STATE-kind (an
// EVENT-only accessor must see nothing for it). Contrast bb_task_config_t.
// core, where 0 is a REAL value and needs its own sentinel instead of
// zero-init (wiki Conventions#api-conventions).
void test_bb_data_http_attach_omitted_kind_matches_pre_collapse_base_call(void)
{
    reset_all();
    bb_data_http_init(NULL);

    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="sk1",.topic="t"}));
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_attach_count());
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_event_ring_count_for_test());

    fake_gen_reset();
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    fake_gen_bump("sk1");
    bb_data_http_push_pump();
    // A STATE-kind key is never fed into the EVENT ring by push_pump() --
    // if kind had silently defaulted to something other than
    // BB_DATA_HTTP_STATE, this key would show up here instead.
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_event_ring_count_for_test());
    bb_data_http_set_generation_fn(NULL, NULL);
    bb_data_http_set_render_fn(NULL, NULL);
}

void test_bb_data_http_attach_null_or_empty_args_return_invalid_arg(void)
{
    reset_all();
    bb_data_http_init(NULL);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key=NULL,.topic="t"}));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k",.topic=NULL}));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="",.topic="t"}));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k",.topic=""}));
}

void test_bb_data_http_attach_key_too_long_returns_invalid_arg(void)
{
    reset_all();
    bb_data_http_init(NULL);

    char key[BB_DATA_HTTP_KEY_MAX + 1];
    memset(key, 'k', sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key=key,.topic="t"}));
}

void test_bb_data_http_attach_topic_too_long_returns_invalid_arg(void)
{
    reset_all();
    bb_data_http_init(NULL);

    char topic[BB_DATA_HTTP_TOPIC_MAX + 1];
    memset(topic, 't', sizeof(topic) - 1);
    topic[sizeof(topic) - 1] = '\0';
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k",.topic=topic}));
}

void test_bb_data_http_attach_capacity_full_returns_no_space(void)
{
    reset_all();
    bb_data_http_init(NULL);

    char keys[BB_DATA_HTTP_MAX_ATTACH + 1][16];
    for (int i = 0; i < BB_DATA_HTTP_MAX_ATTACH; i++) {
        snprintf(keys[i], sizeof(keys[i]), "k.%d", i);
        TEST_ASSERT_EQUAL(BB_OK, bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key=keys[i],.topic="t"}));
    }

    snprintf(keys[BB_DATA_HTTP_MAX_ATTACH], sizeof(keys[BB_DATA_HTTP_MAX_ATTACH]), "k.%d", BB_DATA_HTTP_MAX_ATTACH);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key=keys[BB_DATA_HTTP_MAX_ATTACH],.topic="t"}));

    // Re-attaching an already-attached key still succeeds even with the
    // table full.
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key=keys[0],.topic="t2"}));
}

// B1-1045 PR-4 fix: bb_data_http_attach() rejects (and does NOT
// attach) a key whose snap_size exceeds the render scratch bound instead of
// silently attaching a key that would render-fail forever. Host default
// mirrors the Kconfig default (256, see bb_data_http_common.c's
// CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES bridge).
void test_bb_data_http_attach_sized_oversized_snap_size_returns_no_space_and_does_not_attach(void)
{
    reset_all();
    bb_data_http_init(NULL);

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_attach_count());
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                      bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="oversized",.topic="t",.kind=BB_DATA_HTTP_STATE,.snap_size=257}));
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_attach_count());
}

// Branch coverage for the loud-reject log line's `key ? key : "(null)"`
// argument with a `key ? key : "(null)"` ternary (bb_data_http_common.c) --
// bb_data_http_attach() checks snap_size BEFORE ever validating `key`
// (that check happens later in the same function, never reached on this
// early-reject path), so a NULL key reaching this guard is a real,
// reachable case, not a defensive no-op. Exercises the ternary's NULL arm.
void test_bb_data_http_attach_sized_oversized_snap_size_null_key_does_not_crash(void)
{
    reset_all();
    bb_data_http_init(NULL);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                      bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key=NULL,.topic="t",.kind=BB_DATA_HTTP_STATE,.snap_size=257}));
}

// The "log" key's actual wire size (220 bytes, BB_LOG_EVENT_LOG_TEXT_MAX)
// is the case this fix restores: it now fits the bumped 256-byte scratch
// and attaches successfully instead of being silently starved.
void test_bb_data_http_attach_sized_log_sized_snap_size_attaches_successfully(void)
{
    reset_all();
    bb_data_http_init(NULL);

    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="log",.topic="log",.kind=BB_DATA_HTTP_STATE,.snap_size=220}));
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_attach_count());
}

// The boundary itself (exactly the scratch size) is accepted -- only
// STRICTLY GREATER than the scratch is rejected (bb_data_render()'s own
// `scratch_cap < desc->snap_size` check is likewise a strict `<`).
void test_bb_data_http_attach_sized_exact_scratch_size_attaches_successfully(void)
{
    reset_all();
    bb_data_http_init(NULL);

    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k",.topic="t",.kind=BB_DATA_HTTP_STATE,.snap_size=256}));
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_attach_count());
}

// ---------------------------------------------------------------------------
// Describe table (B1-1220) -- deliberately independent of the attach table
// above; see bb_data_http.h's doc comment. Nothing in this component's own
// logic reads component_name/schema_literal back -- bb_openapi is the sole
// consumer (test_sse_schema_fidelity.c covers the emission side), so these
// tests exercise storage/dedup/overflow/foreach directly via
// bb_data_http_describe_foreach() capturing into a local fixture.
// ---------------------------------------------------------------------------

typedef struct {
    const char *key;
    const char *topic;
    const char *component_name;
    const char *schema_literal;
} describe_capture_t;

#define DESCRIBE_CAPTURE_MAX (BB_DATA_HTTP_MAX_DESCRIBE + 1)

static describe_capture_t s_describe_capture[DESCRIBE_CAPTURE_MAX];
static size_t             s_describe_capture_count;

static void describe_capture_reset(void)
{
    s_describe_capture_count = 0;
    memset(s_describe_capture, 0, sizeof(s_describe_capture));
}

static void describe_capture_cb(const char *key, const char *topic,
                                const char *component_name,
                                const char *schema_literal, void *ctx)
{
    size_t *count = (size_t *)ctx;
    if (*count >= DESCRIBE_CAPTURE_MAX) return;  // LCOV_EXCL_LINE -- fixture never sees more than the table's own cap
    s_describe_capture[*count].key            = key;
    s_describe_capture[*count].topic          = topic;
    s_describe_capture[*count].component_name = component_name;
    s_describe_capture[*count].schema_literal = schema_literal;
    (*count)++;
}

void test_bb_data_http_describe_round_trip_via_foreach(void)
{
    reset_all();
    describe_capture_reset();

    TEST_ASSERT_EQUAL(BB_OK,
        bb_data_http_describe("k1", "topic.a", "Comp1", "{\"type\":\"object\"}"));

    bb_data_http_describe_foreach(describe_capture_cb, &s_describe_capture_count);
    TEST_ASSERT_EQUAL_UINT(1, s_describe_capture_count);
    TEST_ASSERT_EQUAL_STRING("k1", s_describe_capture[0].key);
    TEST_ASSERT_EQUAL_STRING("topic.a", s_describe_capture[0].topic);
    TEST_ASSERT_EQUAL_STRING("Comp1", s_describe_capture[0].component_name);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"object\"}", s_describe_capture[0].schema_literal);
}

// Dedup first-wins: re-describing an already-described key is a no-op --
// the FIRST registration's topic/component_name/schema_literal survive.
void test_bb_data_http_describe_dedup_first_wins(void)
{
    reset_all();
    describe_capture_reset();

    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_describe("k1", "topic.a", "Comp1", "{}"));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_describe("k1", "topic.b", "Comp2", "{\"x\":1}"));

    bb_data_http_describe_foreach(describe_capture_cb, &s_describe_capture_count);
    TEST_ASSERT_EQUAL_UINT(1, s_describe_capture_count);
    TEST_ASSERT_EQUAL_STRING("topic.a", s_describe_capture[0].topic);
    TEST_ASSERT_EQUAL_STRING("Comp1", s_describe_capture[0].component_name);
}

void test_bb_data_http_describe_null_or_empty_args_return_invalid_arg(void)
{
    reset_all();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_describe(NULL, "t", "C", "{}"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_describe("k", NULL, "C", "{}"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_describe("k", "t", NULL, "{}"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_describe("k", "t", "C", NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_describe("", "t", "C", "{}"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_describe("k", "", "C", "{}"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_describe("k", "t", "", "{}"));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_describe("k", "t", "C", ""));
}

void test_bb_data_http_describe_capacity_full_returns_no_space(void)
{
    reset_all();

    char keys[BB_DATA_HTTP_MAX_DESCRIBE + 1][16];
    for (int i = 0; i < BB_DATA_HTTP_MAX_DESCRIBE; i++) {
        snprintf(keys[i], sizeof(keys[i]), "k.%d", i);
        TEST_ASSERT_EQUAL(BB_OK, bb_data_http_describe(keys[i], "t", "C", "{}"));
    }

    snprintf(keys[BB_DATA_HTTP_MAX_DESCRIBE], sizeof(keys[BB_DATA_HTTP_MAX_DESCRIBE]),
             "k.%d", BB_DATA_HTTP_MAX_DESCRIBE);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
        bb_data_http_describe(keys[BB_DATA_HTTP_MAX_DESCRIBE], "t", "C", "{}"));

    // Re-describing an already-described key still succeeds even with the
    // table full.
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_describe(keys[0], "t2", "C2", "{\"y\":2}"));
}

void test_bb_data_http_describe_foreach_null_cb_is_noop(void)
{
    reset_all();

    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_describe("k1", "t", "C", "{}"));
    bb_data_http_describe_foreach(NULL, NULL);  // must not crash
}

void test_bb_data_http_describe_foreach_empty_table_calls_nothing(void)
{
    reset_all();
    describe_capture_reset();

    bb_data_http_describe_foreach(describe_capture_cb, &s_describe_capture_count);
    TEST_ASSERT_EQUAL_UINT(0, s_describe_capture_count);
}

// The attach table and describe table are independent: describing a key
// never attaches it, and attaching a key never describes it.
void test_bb_data_http_describe_independent_of_attach_table(void)
{
    reset_all();
    bb_data_http_init(NULL);
    describe_capture_reset();

    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_describe("k1", "t", "C", "{}"));
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_attach_count());

    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k2",.topic="t2"}));
    bb_data_http_describe_foreach(describe_capture_cb, &s_describe_capture_count);
    TEST_ASSERT_EQUAL_UINT(1, s_describe_capture_count);
    TEST_ASSERT_EQUAL_STRING("k1", s_describe_capture[0].key);
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
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c));
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
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c));
}

void test_bb_data_http_client_acquire_null_out_returns_invalid_arg(void)
{
    reset_all();
    bb_data_http_init(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, NULL));
}

void test_bb_data_http_client_acquire_null_cfg_returns_invalid_arg(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_client_acquire(NULL, &c));
}

void test_bb_data_http_client_acquire_exhaustion_returns_no_space(void)
{
    reset_all();
    bb_data_http_cfg_t cfg = { .max_clients = 2 };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_init(&cfg));

    bb_data_http_client_t *c1 = NULL, *c2 = NULL, *c3 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c1));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c2));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c3));
}

// B1-1447 review LOW 2: proving the error code alone is a weak test -- it
// passes whether or not the poll/push pool slots claimed before the failing
// bb_queue_create() call are actually unwound (bb_data_http_common.c's
// acquire()'s poll_free(c->poll)/push_free(c->push) pair). Since this cfg
// uses the default (ALL) mask, BOTH pools are claimed before the queue
// create fails, so a leaked slot here is a leaked slot in EITHER pool, in a
// static pool with no GC -- permanent once the default cap (2 of each) is
// exhausted by repeated failures. Reclamation is proven, not just the
// return code: after resetting the (now-working) allocator, a second
// default-mask acquire must SUCCEED with both poll and push non-NULL --
// impossible if either free() call above were silently dropped, since the
// pool involved would then already read "exhausted" from this one prior
// failed attempt.
//
// Mutation evidence: delete the poll_free(c->poll)/push_free(c->push) pair
// in bb_data_http_client_acquire()'s outbound-alloc-failure unwind path --
// this test then FAILS (the second acquire returns BB_ERR_NO_SPACE, or
// succeeds with an unexpectedly-NULL poll/push), while the OLD (error-code-
// only) assertion would still have passed.
void test_bb_data_http_client_acquire_outbound_alloc_failure_returns_error(void)
{
    reset_all();
    // Pool caps pinned to exactly 1 each (rather than the Kconfig default
    // of 2): the reclamation proof below only DISCRIMINATES a dropped
    // poll_free()/push_free() call if the single slot the failed attempt
    // claims is the ONLY slot in its pool -- at the default cap of 2, a
    // leaked slot still leaves one spare for the second acquire to draw
    // from, and the test would pass whether or not the unwind actually
    // freed anything (verified by mutation, see this test's own doc below).
    bb_data_http_cfg_t cfg = { .max_clients = 2, .max_poll_clients = 1, .max_push_clients = 1 };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_init(&cfg));

    bb_queue_set_allocator(failing_calloc, free);
    bb_data_http_client_t *failed = NULL;
    bb_err_t               rc = bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &failed);
    bb_queue_reset_allocator();

    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);
    TEST_ASSERT_NULL(failed);

    // B1-1447 review LOW 2: the error code alone is a weak test -- it passes
    // whether or not the poll/push pool slots claimed before the failing
    // bb_queue_create() call are actually unwound
    // (bb_data_http_common.c's acquire()'s poll_free(c->poll)/
    // push_free(c->push) pair). Since this cfg uses the default (ALL) mask,
    // BOTH pools' single slot are claimed before the queue create fails --
    // reclamation is proven here, not just the return code: after resetting
    // the (now-working) allocator, a second default-mask acquire must
    // SUCCEED with both poll and push non-NULL, which is only possible if
    // that one slot in EACH pool was actually returned.
    //
    // Mutation evidence: delete the poll_free(c->poll)/push_free(c->push)
    // pair in bb_data_http_client_acquire()'s outbound-alloc-failure unwind
    // path -- this test then FAILS (the second acquire returns
    // BB_ERR_NO_SPACE), while the OLD (error-code-only) assertion, and this
    // same reclamation proof at the Kconfig-default pool cap of 2, would
    // both still have passed (a single leaked slot never exhausts a
    // 2-slot pool).
    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c));
    TEST_ASSERT_FALSE(bb_data_http_client_poll_is_null_for_test(c));
    TEST_ASSERT_FALSE(bb_data_http_client_push_is_null_for_test(c));
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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k2",.topic="topic.b"});

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){.topic_filter = NULL}, &c));

    TEST_ASSERT_EQUAL_UINT32(0x3u, bb_data_http_client_dirty_mask_for_test(c));
}

void test_bb_data_http_client_topic_filter_empty_string_subscribes_all(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k2",.topic="topic.b"});

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){.topic_filter = ""}, &c));

    TEST_ASSERT_EQUAL_UINT32(0x3u, bb_data_http_client_dirty_mask_for_test(c));
}

void test_bb_data_http_client_topic_filter_exact_match_only(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k2",.topic="topic.b"});

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){.topic_filter = "topic.a"}, &c));

    TEST_ASSERT_EQUAL_UINT32(0x1u, bb_data_http_client_dirty_mask_for_test(c));  // only k1 (attach idx 0)
}

void test_bb_data_http_client_topic_filter_no_match_subscribes_nothing(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){.topic_filter = "topic.nope"}, &c));

    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dirty_mask_for_test(c));
}

// An over-length topic_filter must be REJECTED (BB_ERR_INVALID_ARG), the
// same as bb_data_http_attach() rejects an over-length topic -- never
// silently truncated by bb_strlcpy into a filter that mis-subscribes the
// client to a topic name it never asked for.
void test_bb_data_http_client_acquire_topic_filter_too_long_returns_invalid_arg(void)
{
    reset_all();
    bb_data_http_init(NULL);

    char filter[BB_DATA_HTTP_TOPIC_MAX + 1];
    memset(filter, 't', sizeof(filter) - 1);
    filter[sizeof(filter) - 1] = '\0';

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){.topic_filter = filter}, &c));
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

    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});
    fake_gen_set("k1", 5);

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c));

    bb_data_http_sweep_step();

    TEST_ASSERT_TRUE(s_render_call_count >= 1);
    TEST_ASSERT_EQUAL_STRING("k1", s_last_rendered_key);
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dirty_mask_for_test(c));
    TEST_ASSERT_EQUAL_UINT32(5u, bb_data_http_client_seen_gen_for_test(c, 0));

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
    const bb_data_http_client_t *sent_to = NULL;
    char   buf[128];
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_at(0, &sent_to, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_PTR(c, sent_to);
    buf[len] = '\0';
    TEST_ASSERT_EQUAL_STRING("{\"key\":\"k1\",\"gen\":5}", buf);

    // B1-1123 PR-2: send_fn's `key` argument resolves to the STATE key that
    // was actually rendered.
    char resolved_key[BB_DATA_HTTP_KEY_MAX];
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_key_at(0, resolved_key, sizeof(resolved_key)));
    TEST_ASSERT_EQUAL_STRING("k1", resolved_key);
}

void test_bb_data_http_sweep_step_coalesces_multiple_bumps_into_one_render(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);
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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k2",.topic="topic.b"});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(2, bb_data_http_host_frame_count());

    // B1-1123 PR-2: each frame's `key` resolves to its OWN attach-table
    // entry, not e.g. always the first/last attached key -- exercises
    // resolve_outbound_key() across more than one attach index.
    char key0[BB_DATA_HTTP_KEY_MAX];
    char key1[BB_DATA_HTTP_KEY_MAX];
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_key_at(0, key0, sizeof(key0)));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_key_at(1, key1, sizeof(key1)));
    TEST_ASSERT_EQUAL_STRING("k1", key0);
    TEST_ASSERT_EQUAL_STRING("k2", key1);
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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);

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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});
    fake_gen_set("ev1", 1);

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);

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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});
    fake_gen_set("ev1", 1);

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);

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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);
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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});

    bb_data_http_client_t *a = NULL, *b = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &a));

    fake_gen_bump("ev1");
    bb_data_http_sweep_step();  // delivered to `a` only -- `b` doesn't exist yet
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_event_cursor_for_test(a));
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());

    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &b));
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_event_cursor_for_test(b));  // no backlog

    fake_gen_bump("ev1");
    bb_data_http_sweep_step();  // delivered to BOTH `a` and `b`

    TEST_ASSERT_EQUAL_UINT32(2u, bb_data_http_client_event_cursor_for_test(a));
    TEST_ASSERT_EQUAL_UINT32(2u, bb_data_http_client_event_cursor_for_test(b));
    TEST_ASSERT_EQUAL_UINT(3, bb_data_http_host_frame_count());  // 1 (a only) + 2 (a and b)

    const bb_data_http_client_t *sent_to_a1 = NULL, *sent_to_a2 = NULL, *sent_to_b1 = NULL;
    bb_data_http_host_frame_at(0, &sent_to_a1, NULL, 0, NULL);
    bb_data_http_host_frame_at(1, &sent_to_a2, NULL, 0, NULL);
    bb_data_http_host_frame_at(2, &sent_to_b1, NULL, 0, NULL);
    TEST_ASSERT_EQUAL_PTR(a, sent_to_a1);
    TEST_ASSERT_TRUE(sent_to_a2 == a || sent_to_a2 == b);
    TEST_ASSERT_TRUE(sent_to_b1 == a || sent_to_b1 == b);
    TEST_ASSERT_TRUE(sent_to_a2 != sent_to_b1);  // second event fanned out to BOTH clients
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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev2",.topic="topic.b",.kind=BB_DATA_HTTP_EVENT});

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){.topic_filter = "topic.a"}, &c));

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
    char buf[128];
    size_t len = 0;
    bb_data_http_host_frame_at(0, NULL, buf, sizeof(buf), &len);
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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev0",.topic="t",.kind=BB_DATA_HTTP_EVENT});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="t",.kind=BB_DATA_HTTP_EVENT});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev2",.topic="t",.kind=BB_DATA_HTTP_EVENT});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev3",.topic="t",.kind=BB_DATA_HTTP_EVENT});

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c));
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
    bb_data_http_host_frame_at(0, NULL, buf, sizeof(buf), &len);
    buf[len] = '\0';
    TEST_ASSERT_EQUAL_STRING("{\"dropped\":2}", buf);
    bb_data_http_host_frame_at(1, NULL, buf, sizeof(buf), &len);
    buf[len] = '\0';
    TEST_ASSERT_EQUAL_STRING("{\"key\":\"ev2\",\"gen\":1}", buf);
    bb_data_http_host_frame_at(2, NULL, buf, sizeof(buf), &len);
    buf[len] = '\0';
    TEST_ASSERT_EQUAL_STRING("{\"key\":\"ev3\",\"gen\":1}", buf);

    // B1-1123 PR-2: send_fn's `key` argument -- the drop-marker frame (0)
    // resolves to the reserved literal, never a real attach-table key; the
    // surviving EVENT frames (1, 2) resolve to their own bb_data key.
    char key[BB_DATA_HTTP_KEY_MAX];
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_key_at(0, key, sizeof(key)));
    TEST_ASSERT_EQUAL_STRING("dropped", key);
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_key_at(1, key, sizeof(key)));
    TEST_ASSERT_EQUAL_STRING("ev2", key);
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_key_at(2, key, sizeof(key)));
    TEST_ASSERT_EQUAL_STRING("ev3", key);
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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="t",.kind=BB_DATA_HTTP_EVENT});

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c));

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
    bb_data_http_host_frame_at(8, NULL, buf, sizeof(buf), &len);
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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="t",.kind=BB_DATA_HTTP_EVENT});

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c));

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
    bb_data_http_host_frame_at(8, NULL, buf, sizeof(buf), &len);
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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="s1",.topic="state.only"});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="event.only",.kind=BB_DATA_HTTP_EVENT});

    bb_data_http_client_t *slow = NULL, *fast = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){.topic_filter = NULL}, &slow));            // all topics
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){.topic_filter = "event.only"}, &fast));    // events only

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

// ---------------------------------------------------------------------------
// bb_data_http_push_pump() (B1-1449, epic B1-1123): the ring FEED, extracted
// out of bb_data_http_sweep_step()'s own detect phase so it can be called
// directly, decoupled from the 50ms broadcaster tick. Per the SCOPE FENCE on
// bb_data_http_push_pump()'s own doc (bb_data_http.h), draining the ring
// into a client's outbound queue and the flush/retry state machine both stay
// tick-driven -- these tests never call push_pump() from anywhere but the
// single (fake) broadcaster call sequence itself, mirroring every other
// EVENT test above.
// ---------------------------------------------------------------------------

// Calling bb_data_http_push_pump() directly, twice, with no sweep_step() in
// between, must feed the shared ring twice -- proving the ring feed itself
// no longer needs a sweep_step() call (i.e. the 50ms broadcaster tick) to
// run at all.
void test_bb_data_http_push_pump_called_twice_directly_feeds_ring_twice(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_event_ring_count_for_test());

    fake_gen_bump("ev1");
    bb_data_http_push_pump();
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_event_ring_count_for_test());

    fake_gen_bump("ev1");
    bb_data_http_push_pump();
    TEST_ASSERT_EQUAL_UINT(2, bb_data_http_event_ring_count_for_test());
}

// B1-1440: generalizes the two-call proof above to N sequential lines,
// modeling bb_log_event's forwarder task pattern (platform/espidf/
// bb_log_event/bb_log_event.c s_forwarder_task) -- stash a new value, bump
// the generation, then push_pump() BEFORE the next line's stash overwrites
// it. This is the fast host-level proof that per-line notify_push() (which
// wraps exactly this bump-then-pump sequence) preserves every line as a
// distinct ring entry: a fix that instead bumps N times and pumps ONCE at
// the end (the bug this ticket fixes -- see the STATE-binding regression it
// replaced) would coalesce to a single entry, same as STATE's dirty-mask.
// fake_render_fn() embeds the observed generation in each frame's own JSON
// body, so entry COUNT and per-entry CONTENT both prove no coalescing.
void test_bb_data_http_push_pump_n_sequential_bumps_feed_n_distinct_entries(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="log",.topic="log",.kind=BB_DATA_HTTP_EVENT});

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c));

    const int kLines = 5;
    for (int i = 0; i < kLines; i++) {
        fake_gen_bump("log");     // per-line stash write (generation is the stand-in payload)
        bb_data_http_push_pump(); // per-line drain -- the notify_push() half of the fix
    }
    TEST_ASSERT_EQUAL_UINT((unsigned)kLines, bb_data_http_event_ring_count_for_test());

    bb_data_http_sweep_step();
    TEST_ASSERT_EQUAL_UINT((unsigned)kLines, bb_data_http_host_frame_count());

    char buf[128];
    size_t len = 0;
    for (int i = 0; i < kLines; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_at(i, NULL, buf, sizeof(buf), &len));
        buf[len] = '\0';
        char expect[64];
        snprintf(expect, sizeof(expect), "{\"key\":\"log\",\"gen\":%d}", i + 1);
        TEST_ASSERT_EQUAL_STRING(expect, buf);
    }
}

// After two direct bb_data_http_push_pump() calls (no sweep_step() in
// between -- see above), a SINGLE subsequent sweep_step() call must deliver
// BOTH ring entries to a subscribed client, in order -- proving the feed
// happened independently of, and strictly before, any drain/flush cadence.
// fake_render_fn() embeds the observed generation in the frame's own JSON
// body, so the two frames' content (not just their count) proves ordering.
void test_bb_data_http_push_pump_twice_then_sweep_step_delivers_both_in_order(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c));

    fake_gen_bump("ev1");  // gen=1
    bb_data_http_push_pump();
    fake_gen_bump("ev1");  // gen=2
    bb_data_http_push_pump();
    TEST_ASSERT_EQUAL_UINT(2, bb_data_http_event_ring_count_for_test());
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_host_frame_count());  // not drained/flushed yet

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(2, bb_data_http_host_frame_count());

    char buf0[128], buf1[128];
    size_t len0 = 0, len1 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_at(0, NULL, buf0, sizeof(buf0), &len0));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_at(1, NULL, buf1, sizeof(buf1), &len1));
    buf0[len0] = '\0';
    buf1[len1] = '\0';
    TEST_ASSERT_EQUAL_STRING("{\"key\":\"ev1\",\"gen\":1}", buf0);
    TEST_ASSERT_EQUAL_STRING("{\"key\":\"ev1\",\"gen\":2}", buf1);
}

// generation_fn failure on an EVENT key must skip that key's ring feed
// entirely -- bb_data_http_push_pump()'s own `s_gen_fn(...) != BB_OK`
// branch, now a distinct branch from sweep_step()'s STATE-only detect loop
// since the B1-1449 split (previously one shared branch covered both kinds).
// TWO EVENT keys in the SAME push_pump() call, one whose generation_fn
// succeeds (ev_ok) and one that fails (ev_fail): asserting only "the ring
// stayed empty" would pass identically for a push_pump() that does nothing
// at all, so this asserts BOTH that ev_ok's bump DID land in the ring (ring
// count == 1, positive proof push_pump() actually ran) AND that ev_fail's
// bump specifically did NOT (never advances past 1) -- the failure is
// selective, not a symptom of the whole function being a no-op.
void test_bb_data_http_push_pump_generation_fn_failure_skips_event_key(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_set_generation_fn(fake_generation_fn_with_failure, NULL);
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev_ok",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev_fail",.topic="topic.b",.kind=BB_DATA_HTTP_EVENT});

    s_gen_fail_key = "ev_fail";
    fake_gen_bump("ev_ok");
    fake_gen_bump("ev_fail");
    bb_data_http_push_pump();

    // Positive proof push_pump() ran at all: ev_ok's bump landed.
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_event_ring_count_for_test());

    // A second push_pump() call with no further bumps must not grow the
    // ring further: ev_fail is still failing (s_gen_fail_key unchanged) and
    // ev_ok's generation hasn't moved again since its last successful push.
    bb_data_http_push_pump();
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_event_ring_count_for_test());
}

// B1-1450: the claim/commit/revoke protocol's compare-and-restore step
// must NOT clobber a NEWER generation a concurrent caller has since
// claimed for the same key -- see this function's own doc
// (bb_data_http.h) and concurrent_claim_then_fail_render_fn()'s doc above
// for the exact race this simulates. Without the compare (i.e. a bare
// unconditional restore-on-failure), this would wrongly resurrect the
// stale pre-claim generation over the concurrent caller's newer claim.
void test_bb_data_http_push_pump_revoke_leaves_newer_concurrent_claim_alone(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(concurrent_claim_then_fail_render_fn, NULL);
    // ev0 attached FIRST so the test-only key->index lookup helper
    // (find_attach_index_for_test(), bb_data_http_common.c) must scan past
    // a non-matching entry before finding "ev1" -- exercises its strcmp
    // mismatch branch, not just an always-first-match lookup.
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev0",.topic="topic.z",.kind=BB_DATA_HTTP_EVENT});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});

    fake_gen_bump("ev1");  // gen=1
    bb_data_http_push_pump();

    // render_fn always fails in this fixture -- nothing ever committed.
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_event_ring_count_for_test());

    // The revoke found s_event_last_gen already moved to 2 (by the
    // simulated concurrent caller inside render_fn, BEFORE this caller's
    // own revoke ran) and correctly left it there instead of restoring it
    // to the pre-claim value (0).
    TEST_ASSERT_EQUAL_UINT32(2u, bb_data_http_event_last_gen_for_test("ev1"));
}

// B1-1450 round 2: COMMIT's own compare-and-discard must reject a
// stale-but-successful render once a NEWER concurrent claim has already
// landed for the same key -- see push_pump()'s own doc (bb_data_http.h,
// step 4a) and concurrent_claim_then_succeed_render_fn()'s doc above for
// the exact race this simulates. Proves the DISCARD itself (ring does not
// grow), not merely that the generation marker is left alone -- distinct
// from test_bb_data_http_push_pump_revoke_leaves_newer_concurrent_claim_alone
// above, which covers the REVOKE (render-failure) side of this same
// symmetry.
void test_bb_data_http_push_pump_commit_discards_stale_render_after_newer_concurrent_claim(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(concurrent_claim_then_succeed_render_fn, NULL);
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});

    fake_gen_bump("ev1");  // gen=1
    bb_data_http_push_pump();

    // render_fn SUCCEEDED (unlike the REVOKE-side test above), but by the
    // time COMMIT re-checked the claim, the simulated concurrent caller had
    // already claimed gen=2 for this same key -- COMMIT must discard this
    // stale render rather than push a second, duplicate frame.
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_event_ring_count_for_test());
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_render_fail_count());  // a discard is not a render failure
    TEST_ASSERT_EQUAL_UINT32(2u, bb_data_http_event_last_gen_for_test("ev1"));  // the newer claim stands, untouched
}

// Tick-driven behavior is unchanged (mutation-test target, B1-1449): with no
// direct bb_data_http_push_pump() call at all, a plain bb_data_http_sweep_step()
// call must still feed the ring AND deliver it to a subscribed client within
// the SAME call -- exactly as before this extraction (mirrors
// test_bb_data_http_sweep_step_event_kind_key_delivers_via_shared_ring
// above, restated here to sit next to push_pump()'s own dedicated tests).
void test_bb_data_http_sweep_step_alone_still_feeds_and_delivers_event(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c));

    fake_gen_bump("ev1");
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_event_ring_count_for_test());
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
}

void test_bb_data_http_sweep_step_without_render_fn_still_clears_dirty(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    // no render_fn installed
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);

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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);

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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);

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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_client_outbound_count_for_test(c));
}

// ---------------------------------------------------------------------------
// B1-1424/B1-1429: the flush phase's retriable-vs-fatal send-retry contract
// (distinct from the render-retry contract above). send_fn's return decides
// whether a failure is safe to retry (BB_ERR_TIMEOUT -- nothing was
// transmitted) or must abort the connection immediately (anything else --
// bb_data_http_send_fn's doc, bb_data_http.h). Mutation-tested per test (see
// each test's comment).
// ---------------------------------------------------------------------------

// Mutation evidence: reverting the flush loop to its pre-fix shape (pop
// unconditionally regardless of send_rc) makes this FAIL -- outbound_count
// would read 0 and send_fail_count 0 after the first (failing) sweep,
// because the frame was discarded instead of retried.
void test_bb_data_http_sweep_step_send_failure_retriable_leaves_frame_queued_and_retries_next_sweep(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);  // fresh-render-on-connect dirties k1

    bb_data_http_host_fail_next(BB_ERR_TIMEOUT, 1);

    bb_data_http_sweep_step();  // render succeeds (bit clears, frame queued); send fails (retriable)

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_host_frame_count());  // never delivered
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_client_outbound_count_for_test(c));  // still queued
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_send_fail_count_for_test(c));

    // No further failure injected -- the SAME frame, untouched since sweep
    // 1, must now go out.
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());  // retried and delivered
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_client_outbound_count_for_test(c));
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_send_fail_count_for_test(c));  // reset on success
}

// Two dirty STATE keys render into the SAME client's outbound queue in one
// sweep; the first one's send fails retriably. Proves the flush loop stops
// for this client for the rest of THIS sweep_step() call rather than
// skipping ahead to the second (already-queued) frame -- both preserving
// delivery order and keeping a stalled client's per-sweep work bounded (see
// bb_data_http_sweep_step()'s flush doc). Mutation evidence: changing the
// loop's `break` (on retriable send_rc, below bound) to `continue` makes
// this FAIL -- frame_count would read 1 (k2 sent out of order, ahead of the
// still-queued k1) instead of 0.
void test_bb_data_http_sweep_step_send_failure_retriable_does_not_retry_within_same_sweep(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k2",.topic="topic.a"});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);  // fresh-render-on-connect dirties both k1 and k2

    // Only ONE failure injected: if k2's send were ever attempted this
    // sweep, the injected failure would already be spent on k1 and k2's
    // send would succeed (getting captured) -- so a captured frame here
    // would prove k2 WAS attempted, contradicting the no-loop-within-a-
    // sweep contract.
    bb_data_http_host_fail_next(BB_ERR_TIMEOUT, 1);

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_host_frame_count());  // neither frame reached send_fn's capture
    TEST_ASSERT_EQUAL_UINT(2, bb_data_http_client_outbound_count_for_test(c));  // both still queued, untouched
}

// Exceeding CONFIG_BB_DATA_HTTP_SEND_FAIL_MAX (default 3, no Kconfig on
// host so the host build always sees the C default) consecutive RETRIABLE
// failures on the SAME head-of-queue frame drops that one frame and resets
// the counter, so a permanently-wedged transport cannot fill this client's
// outbound queue and stall it forever (see bb_data_http_sweep_step()'s
// flush doc). Mutation evidence: removing the
// `>= CONFIG_BB_DATA_HTTP_SEND_FAIL_MAX` drop branch (i.e. always `break`,
// unconditionally) makes this FAIL -- outbound_count would still read 1
// after the 3rd failing sweep instead of 0, and frame_count would never
// move off 0 for k1 either since the permanently-wedged head frame would
// never leave the queue.
void test_bb_data_http_sweep_step_send_failure_retriable_bound_exceeded_drops_frame(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);  // fresh-render-on-connect dirties k1

    bb_data_http_host_fail_next(BB_ERR_TIMEOUT, 3);

    bb_data_http_sweep_step();  // failure 1/3 -- still under bound, frame stays queued
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_client_outbound_count_for_test(c));
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_send_fail_count_for_test(c));

    bb_data_http_sweep_step();  // failure 2/3 -- still under bound, frame stays queued
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_client_outbound_count_for_test(c));
    TEST_ASSERT_EQUAL_UINT32(2u, bb_data_http_client_send_fail_count_for_test(c));

    bb_data_http_sweep_step();  // failure 3/3 -- bound reached, frame dropped
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_client_outbound_count_for_test(c));  // dropped, not stuck
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_send_fail_count_for_test(c));  // counter reset
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_host_frame_count());  // never actually delivered

    // The queue is unblocked: a fresh update for the same key now sends
    // normally, proving this client did not stall permanently.
    fake_gen_bump("k1");
    bb_data_http_sweep_step();
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_client_outbound_count_for_test(c));
}

// B1-1424 review MEDIUM fix: a bound-exceeded drop must NOT immediately
// attempt the next queued frame within the same sweep_step() call -- the
// doc previously overstated the per-sweep budget guarantee (KB gap: no
// existing test attached more than one key on this path, so it was
// unexercised). k1 (head) trips the bound and is dropped on the 3rd sweep;
// k2 (already queued behind it) must stay untouched THIS sweep and only go
// out on the NEXT one. Mutation evidence: reverting the bound-exceeded
// branch's `break` back to `continue` (i.e. immediately attempting k2 in
// the same call after dropping k1) makes this FAIL -- frame_count would
// read 1 (k2 delivered on the same sweep as k1's drop) instead of 0, and
// outbound_count would read 0 instead of 1.
void test_bb_data_http_sweep_step_send_failure_retriable_bound_exceeded_does_not_attempt_next_frame_same_sweep(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k2",.topic="topic.a"});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);  // fresh-render-on-connect dirties both k1 and k2

    bb_data_http_host_fail_next(BB_ERR_TIMEOUT, 3);

    bb_data_http_sweep_step();  // k1 failure 1/3
    bb_data_http_sweep_step();  // k1 failure 2/3
    bb_data_http_sweep_step();  // k1 failure 3/3 -- bound reached, k1 dropped

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_client_outbound_count_for_test(c));  // k2 still queued, untouched
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_host_frame_count());  // k2 never attempted this sweep either

    // Next sweep, no injected failure: k2 goes out normally.
    bb_data_http_sweep_step();
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_client_outbound_count_for_test(c));

    char key0[BB_DATA_HTTP_KEY_MAX];
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_key_at(0, key0, sizeof(key0)));
    TEST_ASSERT_EQUAL_STRING("k2", key0);
}

// A FATAL (non-BB_ERR_TIMEOUT) send_fn failure never touches
// send_fail_count -- it tears the client down immediately via the
// installed abort_fn instead. Mutation evidence: routing a fatal send_rc
// through the retriable branch (i.e. treating every non-BB_OK code as
// BB_ERR_TIMEOUT) makes this FAIL -- the abort_fn would never fire,
// bb_data_http_host_last_aborted_client() would stay NULL, and the client
// would remain active (queued for a next-sweep retry) instead of torn down.
void test_bb_data_http_sweep_step_send_failure_fatal_invokes_abort_fn(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_host_install_abort();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);  // fresh-render-on-connect dirties k1

    bb_data_http_host_fail_next(BB_ERR_INVALID_ARG, 1);  // non-BB_ERR_TIMEOUT == fatal

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_PTR(c, bb_data_http_host_last_aborted_client());
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_host_frame_count());  // never delivered
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_send_fail_count_for_test(NULL));  // counter untouched (client gone)
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_active_client_count());  // torn down, not left active
}

// With NO abort_fn installed, a fatal send_fn failure degrades to the
// core's own bb_data_http_client_release() fallback (see
// bb_data_http_set_abort_fn()'s doc) -- the client is still torn down, just
// without a backend-specific teardown callback. Mutation evidence: removing
// the `else bb_data_http_client_release(c)` fallback branch makes this FAIL
// -- active_client_count() would still read 1 (the client leaked, queued
// forever with no send_fn ever able to reach it again since the frame stays
// at the head of a queue nothing drains).
void test_bb_data_http_sweep_step_send_failure_fatal_without_abort_fn_releases_client(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    // no abort_fn installed
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);  // fresh-render-on-connect dirties k1
    (void)c;

    bb_data_http_host_fail_next(BB_ERR_INVALID_ARG, 1);  // fatal

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_active_client_count());
}

// A client acquired with its OWN abort_fn/abort_ctx (bb_data_http_client_cfg_t)
// must use THAT seam on a fatal send_fn failure, never the module-wide
// default installed via bb_data_http_set_abort_fn() -- proves the abort seam
// resolves per-client-then-global, same as send_fn (B1-1123 PR-1).
static bb_data_http_client_t *s_per_client_aborted;

static void per_client_abort(bb_data_http_client_t *client, void *ctx)
{
    (void)ctx;
    s_per_client_aborted = client;
    bb_data_http_client_release(client);
}

void test_bb_data_http_sweep_step_send_failure_fatal_uses_per_client_abort_fn_over_global(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_host_install_abort();  // module-wide default -- must NOT fire for this client
    s_per_client_aborted = NULL;
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});

    bb_data_http_client_t *c = NULL;
    // fresh-render-on-connect dirties k1
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){.abort_fn = per_client_abort}, &c);

    bb_data_http_host_fail_next(BB_ERR_INVALID_ARG, 1);  // non-BB_ERR_TIMEOUT == fatal

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_PTR(c, s_per_client_aborted);
    TEST_ASSERT_NULL(bb_data_http_host_last_aborted_client());  // module-wide abort_fn never invoked
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_active_client_count());
}

// ---------------------------------------------------------------------------
// B1-1424 HIGH fix: deferred reap. bb_data_http_client_release() is NOT
// cross-task-safe (see its doc) -- a foreign task (e.g. the espidf
// backend's ws_disconnect_cb, which fires on bb_ws_server's own worker
// task, never the broadcaster) must use
// bb_data_http_client_request_release() instead, which only flips an
// atomic flag; the actual bb_data_http_client_release() call happens on
// the OWNING task's next sweep_step() call, never mid-sweep and never from
// any other task -- see bb_data_http_client_t's TASK OWNERSHIP doc.
// ---------------------------------------------------------------------------

// The basic contract in isolation, no sweep in flight: requesting release
// does not itself release the client (still active, still counted) -- only
// the NEXT sweep_step() call does. Mutation evidence: removing the
// pending_release reap check from sweep_step()'s per-client loop makes
// this FAIL -- active_client_count() would still read 1 after the
// sweep_step() call instead of 0.
void test_bb_data_http_client_request_release_defers_actual_release_to_next_sweep(void)
{
    reset_all();
    bb_data_http_init(NULL);

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_active_client_count());
    TEST_ASSERT_FALSE(bb_data_http_client_pending_release_for_test(c));

    bb_data_http_client_request_release(c);

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_active_client_count());  // NOT yet released
    TEST_ASSERT_TRUE(bb_data_http_client_pending_release_for_test(c));

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_active_client_count());  // reaped now
}

// The concurrent-disconnect-during-drain scenario itself (see
// mid_sweep_release_render_fn()'s own doc above): a request-release call
// that lands PARTWAY THROUGH a sweep_step() call already in progress for
// this client must not corrupt or truncate that in-progress pass -- the
// frame already being rendered this sweep still reaches the wire normally
// (proving the mid-sweep flag flip did not touch anything render_fn/
// send_fn were already using), and the client is reaped -- released,
// uncounted, no crash -- only on the FOLLOWING sweep_step() call, never the
// one the request raced into. Mutation evidence: reaping eagerly WITHIN
// the same sweep_step() call the request lands in (instead of deferring to
// the next one) makes this FAIL -- frame_count would read 0 instead of 1
// (the in-flight render/send for k1 would never complete).
void test_bb_data_http_sweep_step_reaps_client_only_on_sweep_after_mid_sweep_release_request(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);  // fresh-render-on-connect dirties k1

    // ctx == c: mid_sweep_release_render_fn() requests release on THIS
    // client from inside its own render call, simulating a concurrent
    // disconnect landing mid-sweep.
    bb_data_http_set_render_fn(mid_sweep_release_render_fn, c);

    bb_data_http_sweep_step();  // the sweep the request races into

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());  // in-flight render/send for k1 completed normally
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_active_client_count());  // NOT reaped yet -- deferred
    TEST_ASSERT_TRUE(bb_data_http_client_pending_release_for_test(c));

    bb_data_http_sweep_step();  // the NEXT sweep -- reaps it

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_active_client_count());
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());  // unchanged -- nothing else was ever sent
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
// phase records poll->seen_gen from the pre-render generation and runs to
// completion before any render call -- see bb_data_http.h's HARD INVARIANT
// comment and bb_data_http_sweep_step()'s doc.)
// ---------------------------------------------------------------------------

void test_bb_data_http_sweep_step_render_failure_retries_on_next_sweep(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});
    fake_gen_set("k1", 5);

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);  // fresh-render-on-connect sets the dirty bit

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
// no dirty bit, no poll->seen_gen update, no render call -- rather than
// treating the failure as "unchanged" or crashing on an uninitialized `gen`.
// ---------------------------------------------------------------------------

void test_bb_data_http_sweep_step_generation_fn_failure_skips_key(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_set_generation_fn(fake_generation_fn_with_failure, NULL);
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});
    fake_gen_set("k1", 5);

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);
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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});
    fake_gen_set("k1", 1);

    bb_data_http_client_t *sub = NULL, *unsub = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){.topic_filter = "topic.a"}, &sub));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){.topic_filter = "topic.b"}, &unsub));

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
// subscribe_mask (B1-1445): a kind axis orthogonal to topic_filter --
// client_subscribes() must AND both gates, so a consumer can declare "EVENT
// only, never STATE" (or vice versa) instead of that only happening by
// accident of which keys share a topic name.
// ---------------------------------------------------------------------------

// An EVENT-only client (subscribe_mask=BB_DATA_HTTP_SUBSCRIBE_EVENT) whose
// topic_filter matches a STATE-kind key's topic must NOT receive that key --
// under today's topic-only filtering it would (client_subscribes() ignored
// kind entirely). Proven two ways: the fresh-render-on-connect dirty bit
// stays clear at acquire time, and a later generation bump never queues an
// outbound frame either.
void test_bb_data_http_subscribe_mask_event_only_client_excludes_state_key(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="sk1",.topic="topic.a"});  // BB_DATA_HTTP_STATE (default kind)
    fake_gen_set("sk1", 1);

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(
        &(bb_data_http_client_cfg_t){.topic_filter = "topic.a",
                                      .subscribe_mask = BB_DATA_HTTP_SUBSCRIBE_EVENT}, &c));
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dirty_mask_for_test(c));  // never force-dirtied

    fake_gen_bump("sk1");
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_client_outbound_count_for_test(c));
}

// Mirror case: a STATE-only client must not receive an EVENT-kind key under
// its subscribed topic.
void test_bb_data_http_subscribe_mask_state_only_client_excludes_event_key(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(
        &(bb_data_http_client_cfg_t){.topic_filter = "topic.a",
                                      .subscribe_mask = BB_DATA_HTTP_SUBSCRIBE_STATE}, &c));

    fake_gen_bump("ev1");
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_host_frame_count());
}

// Zero-default proof: a cfg that never sets subscribe_mask resolves to
// BB_DATA_HTTP_SUBSCRIBE_ALL -- the client still receives both a STATE key
// (fresh-render-on-connect dirty bit set) and an EVENT key (delivered on a
// later generation bump), matching every existing call site's unchanged
// behavior.
void test_bb_data_http_subscribe_mask_zero_default_receives_both_kinds(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="sk1",.topic="topic.a"});                        // BB_DATA_HTTP_STATE
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});
    fake_gen_set("sk1", 1);

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(
        &(bb_data_http_client_cfg_t){.topic_filter = "topic.a"}, &c));  // subscribe_mask unset
    TEST_ASSERT_EQUAL_UINT32(0x1u, bb_data_http_client_dirty_mask_for_test(c));  // sk1 force-dirtied -> ALL includes STATE

    bb_data_http_sweep_step();  // drains sk1's connect-dirty render
    bb_data_http_host_reset();

    fake_gen_bump("ev1");
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());  // ev1 delivered too -- zero-default == ALL
}

// ---------------------------------------------------------------------------
// B1-1446: two changes bundled here.
//
// 1. The STATE-side skips below (bb_data_http_client_acquire()'s
//    force-dirty walk and bb_data_http_sweep_step()'s detect-phase inner
//    client loop) hoist the subscribe_mask check to run once per client
//    instead of once per (key, client) pair. client_subscribes() already
//    ANDs this same mask bit (B1-1445), so poll->seen_gen/poll->dirty_mask
//    were already left untouched for a mask-excludes-STATE client either
//    way -- there is no NEW observably-different behavior today, and no
//    test below claims one. The hoist exists as a PREREQUISITE for B1-1447
//    (splits STATE bookkeeping into a `poll` pointer, NULL for a
//    mask-excludes-STATE client -- see the skip sites' own comments in
//    bb_data_http_common.c for why visiting at all would then be a NULL
//    dereference).
// 2. drain_client_events() is now skipped ENTIRELY for a mask-excludes-EVENT
//    client -- this one IS a real, previously-absent non-participation gap:
//    that function unconditionally advances event_cursor/drop-accounting
//    off the shared ring's own state, independent of any per-entry
//    client_subscribes() filtering inside it. The test below proves this
//    discriminates participation from delivery: a participating-but-
//    filtered client would still have event_cursor mutated even though
//    nothing was ever delivered to it.
// ---------------------------------------------------------------------------

// Delivery-gate regression guard (B1-1445): an EVENT-only client's
// poll->seen_gen must stay at 0 across a sweep that bumps every attached
// STATE key's generation -- this is NOT a non-participation proof (the mask
// check inside client_subscribes() already guaranteed this before B1-1446's
// hoist landed; see the file-header comment above), just a guard against
// regressing that existing delivery-gate behavior. A default-mask (ALL)
// client in the same sweep proves nothing over-applies: its poll->seen_gen
// DOES advance for the same keys.
void test_bb_data_http_subscribe_mask_event_only_client_state_seen_gen_stays_gated(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="sk1",.topic="topic.a"});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="sk2",.topic="topic.b"});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="sk3",.topic="topic.c"});
    fake_gen_set("sk1", 1);
    fake_gen_set("sk2", 1);
    fake_gen_set("sk3", 1);

    bb_data_http_client_t *push_only = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(
        &(bb_data_http_client_cfg_t){.subscribe_mask = BB_DATA_HTTP_SUBSCRIBE_EVENT}, &push_only));
    bb_data_http_client_t *all = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(
        &(bb_data_http_client_cfg_t){.subscribe_mask = BB_DATA_HTTP_SUBSCRIBE_ALL}, &all));

    fake_gen_bump("sk1");
    fake_gen_bump("sk2");
    fake_gen_bump("sk3");
    bb_data_http_sweep_step();

    for (size_t k = 0; k < 3; k++) {
        TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_seen_gen_for_test(push_only, k));
    }
    // the ALL client isn't over-skipped -- its seen_gen tracks the bumped
    // generations (2, matching each fake_gen_bump() above).
    for (size_t k = 0; k < 3; k++) {
        TEST_ASSERT_EQUAL_UINT32(2u, bb_data_http_client_seen_gen_for_test(all, k));
    }
}

// Non-participation proof (B1-1446): a STATE-only client must never
// participate in EVENT draining at all. Its event_cursor must stay exactly
// where it was set at acquire time (s_event_total_pushed, 0 here) across a
// sweep that pushes several ring entries -- drain_client_events() must
// never even be called for it. A default-mask (ALL) client in the same
// sweep proves the skip doesn't over-apply: its event_cursor DOES advance.
void test_bb_data_http_subscribe_mask_state_only_client_never_advances_event_cursor(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});
    fake_gen_set("ev1", 0);

    bb_data_http_client_t *state_only = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(
        &(bb_data_http_client_cfg_t){.subscribe_mask = BB_DATA_HTTP_SUBSCRIBE_STATE}, &state_only));
    bb_data_http_client_t *all = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(
        &(bb_data_http_client_cfg_t){.subscribe_mask = BB_DATA_HTTP_SUBSCRIBE_ALL}, &all));

    uint32_t cursor_before = bb_data_http_client_event_cursor_for_test(state_only);

    fake_gen_bump("ev1");
    bb_data_http_sweep_step();
    fake_gen_bump("ev1");
    bb_data_http_sweep_step();
    fake_gen_bump("ev1");
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT32(cursor_before, bb_data_http_client_event_cursor_for_test(state_only));
    // the ALL client isn't over-skipped -- its cursor advances past every
    // pushed ring entry.
    TEST_ASSERT_EQUAL_UINT32(cursor_before + 3, bb_data_http_client_event_cursor_for_test(all));
}

// ---------------------------------------------------------------------------
// STATE/EVENT pool split (B1-1447, epic B1-1123): poll/push bookkeeping now
// lives in separately-sized pools, pool-allocated per client at acquire time
// per subscribe_mask bit -- direct proof (not just inferred through a
// NULL-safe accessor's zero-return) that a kind-excluded client's pointer is
// genuinely NULL, plus the pool-exhaustion failure mode this split
// introduces: acquiring more of one kind than that kind's pool holds must
// fail cleanly (BB_ERR_NO_SPACE) even while fd-table client slots remain
// free, never return a client with an unexpectedly-NULL poll/push.
// ---------------------------------------------------------------------------

// Mirror of test_bb_data_http_subscribe_mask_event_only_client_state_seen_gen_
// stays_gated, but a DIRECT poll==NULL proof (bb_data_http_client_poll_is_
// null_for_test()) rather than inferring it from a NULL-safe getter's
// zero-return, plus confirmation the client still sweeps/drains its EVENT
// traffic correctly with no poll block at all.
void test_bb_data_http_push_only_client_has_null_poll_and_drains_correctly(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(
        &(bb_data_http_client_cfg_t){.subscribe_mask = BB_DATA_HTTP_SUBSCRIBE_EVENT}, &c));
    TEST_ASSERT_TRUE(bb_data_http_client_poll_is_null_for_test(c));
    TEST_ASSERT_FALSE(bb_data_http_client_push_is_null_for_test(c));

    fake_gen_bump("ev1");
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_event_cursor_for_test(c));
}

// Mirror case: a STATE-only client's push must be NULL, and it still
// sweeps/drains its STATE traffic correctly with no push block at all.
void test_bb_data_http_poll_only_client_has_null_push_and_drains_correctly(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="sk1",.topic="topic.a"});
    fake_gen_set("sk1", 1);

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(
        &(bb_data_http_client_cfg_t){.subscribe_mask = BB_DATA_HTTP_SUBSCRIBE_STATE}, &c));
    TEST_ASSERT_FALSE(bb_data_http_client_poll_is_null_for_test(c));
    TEST_ASSERT_TRUE(bb_data_http_client_push_is_null_for_test(c));
    TEST_ASSERT_EQUAL_UINT32(0x1u, bb_data_http_client_dirty_mask_for_test(c));  // fresh-render-on-connect
    // bb_data_http_client_dropped_count() must degrade to 0 for a push==NULL
    // client (branch coverage for its `c && c->push` gate's push==NULL arm,
    // distinct from the c==NULL arm test_bb_data_http_for_test_helpers_
    // defend_against_null_and_out_of_range already covers).
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dropped_count(c));

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_dirty_mask_for_test(c));
}

// poll_is_null_for_test()/push_is_null_for_test() must tolerate a NULL
// client the same way every other _for_test() getter does (see
// test_bb_data_http_for_test_helpers_defend_against_null_and_out_of_range) --
// "true" (poll/push IS null) is the correct answer for "there is no client
// to point at", not a crash.
void test_bb_data_http_poll_push_is_null_for_test_null_client_returns_true(void)
{
    reset_all();
    TEST_ASSERT_TRUE(bb_data_http_client_poll_is_null_for_test(NULL));
    TEST_ASSERT_TRUE(bb_data_http_client_push_is_null_for_test(NULL));
}

// Pool exhaustion (B1-1447 CRITICAL): bb_data_http_cfg_t's max_poll_clients/
// max_push_clients shrink-only override lets a host test size a kind's pool
// SMALLER than max_clients -- exactly the "someone tunes a pool down"
// scenario the pool split introduces as a genuinely new failure mode.
// Acquiring a second STATE-including client here must fail BB_ERR_NO_SPACE
// even though a second fd-table client slot is still free (max_clients=2),
// proving pool exhaustion is a DISTINCT failure from client-slot exhaustion,
// never a silent NULL poll on a client that otherwise looks acquired.
void test_bb_data_http_client_acquire_poll_pool_exhausted_returns_no_space(void)
{
    reset_all();
    bb_data_http_cfg_t cfg = { .max_clients = 2, .max_poll_clients = 1 };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_init(&cfg));

    bb_data_http_client_t *c1 = NULL, *c2 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c1));  // default mask ALL -- consumes the one poll slot
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_active_client_count());

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c2));
    TEST_ASSERT_NULL(c2);  // never touched -- caller's prior value survives a failed acquire
    // The fd-table client slot the failed attempt would have used was never
    // consumed: active_client_count() stays at 1, not silently 2 with a
    // half-populated client.
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_active_client_count());

    // Pools are independent: an EVENT-only client still succeeds even with
    // the poll pool fully exhausted.
    bb_data_http_client_t *c3 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(
        &(bb_data_http_client_cfg_t){.subscribe_mask = BB_DATA_HTTP_SUBSCRIBE_EVENT}, &c3));
    TEST_ASSERT_EQUAL_UINT(2, bb_data_http_active_client_count());

    // Releasing the poll-holding client frees its pool slot back -- a
    // subsequent STATE-including acquire then succeeds again, proving the
    // pool is a genuine reusable resource, not a one-shot cap.
    bb_data_http_client_release(c1);
    bb_data_http_client_t *c4 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c4));
    TEST_ASSERT_FALSE(bb_data_http_client_poll_is_null_for_test(c4));
}

// Mirror of the poll-exhaustion test above for the push pool.
void test_bb_data_http_client_acquire_push_pool_exhausted_returns_no_space(void)
{
    reset_all();
    bb_data_http_cfg_t cfg = { .max_clients = 2, .max_push_clients = 1 };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_init(&cfg));

    bb_data_http_client_t *c1 = NULL, *c2 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c1));  // default mask ALL -- consumes the one push slot
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_active_client_count());

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c2));
    TEST_ASSERT_NULL(c2);
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_active_client_count());

    // Pools are independent: a STATE-only client still succeeds even with
    // the push pool fully exhausted.
    bb_data_http_client_t *c3 = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(
        &(bb_data_http_client_cfg_t){.subscribe_mask = BB_DATA_HTTP_SUBSCRIBE_STATE}, &c3));
    TEST_ASSERT_EQUAL_UINT(2, bb_data_http_active_client_count());
}

// Init-time validation: max_poll_clients/max_push_clients may only SHRINK
// the compile-time Kconfig cap, never grow it -- mirrors max_clients/
// event_ring_capacity's own identical over-cap rejection
// (test_bb_data_http_init_max_clients_over_cap_returns_invalid_arg /
// test_bb_data_http_init_event_ring_capacity_over_cap_returns_invalid_arg).
void test_bb_data_http_init_max_poll_clients_over_cap_returns_invalid_arg(void)
{
    reset_all();
    bb_data_http_cfg_t cfg = { .max_poll_clients = 1000 };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_init(&cfg));
}

void test_bb_data_http_init_max_push_clients_over_cap_returns_invalid_arg(void)
{
    reset_all();
    bb_data_http_cfg_t cfg = { .max_push_clients = 1000 };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_data_http_init(&cfg));
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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k2",.topic="topic.b"});

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){.topic_filter = "topic.a"}, &c));
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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);

    for (int i = 0; i < 32; i++) {
        bb_data_http_sweep_step();
    }

    TEST_ASSERT_EQUAL_UINT(32, bb_data_http_render_fail_count());
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_dirty_mask_for_test(c));  // still retrying
}

// Same rate-limit-both-outcomes coverage as the STATE test above, but for
// the EVENT-kind ring-feed's own (separate source line) render-fail
// rate-limit check -- gcov's per-line branch identity means STATE's 32-sweep
// drive above does NOT also exercise this line.
void test_bb_data_http_sweep_step_event_render_failure_log_rate_limit_both_outcomes(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(failing_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});

    for (int i = 0; i < 32; i++) {
        fake_gen_bump("ev1");
        bb_data_http_sweep_step();
    }

    TEST_ASSERT_EQUAL_UINT(32, bb_data_http_render_fail_count());
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_host_frame_count());  // every attempt failed -- nothing ever sent
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
    TEST_ASSERT_FALSE(bb_data_http_client_pending_release_for_test(NULL));
    bb_data_http_client_request_release(NULL);  // no-op, must not crash
    // B1-1449: before init() (or after reset_for_test(), as here) the shared
    // EVENT ring does not exist yet -- the ring-not-created branch.
    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_event_ring_count_for_test());

    bb_data_http_init(NULL);
    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);
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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);

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
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});
    fake_gen_set("k1", 1);

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
    const bb_data_http_client_t *sent_to = NULL;
    char   buf[8];
    size_t len   = 99;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_at(0, &sent_to, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_PTR(c, sent_to);
    TEST_ASSERT_EQUAL_UINT(0, len);
}

void test_bb_data_http_host_frame_at_out_of_range_returns_not_found(void)
{
    reset_all();
    bb_data_http_host_install_send();

    const bb_data_http_client_t *sent_to = NULL;
    char   buf[8];
    size_t len   = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_data_http_host_frame_at(0, &sent_to, buf, sizeof(buf), &len));
}

void test_bb_data_http_host_frame_at_null_out_params_are_optional(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});
    fake_gen_set("k1", 1);

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
    // Every output pointer may be NULL to skip that field (see
    // bb_data_http_host.h) -- must not crash / must still succeed.
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_at(0, NULL, NULL, 0, NULL));

    // A non-NULL buf with buf_cap == 0 must also skip the copy (the `buf &&
    // buf_cap > 0` guard's other half) rather than writing past a
    // zero-capacity buffer.
    char zero_cap_buf[1];
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_at(0, NULL, zero_cap_buf, 0, NULL));
}

void test_bb_data_http_host_frame_key_at_out_of_range_returns_not_found(void)
{
    reset_all();
    bb_data_http_host_install_send();

    char buf[BB_DATA_HTTP_KEY_MAX];
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_data_http_host_frame_key_at(0, buf, sizeof(buf)));
}

void test_bb_data_http_host_frame_key_at_null_buf_is_optional(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});
    fake_gen_set("k1", 1);

    bb_data_http_client_t *c = NULL;
    bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c);
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
    // buf==NULL and buf_cap==0 must both skip the copy without crashing --
    // mirrors bb_data_http_host_frame_at()'s own NULL-optional contract.
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_key_at(0, NULL, sizeof(char) * BB_DATA_HTTP_KEY_MAX));
    char zero_cap_buf[1];
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_host_frame_key_at(0, zero_cap_buf, 0));
}

// ---------------------------------------------------------------------------
// Per-consumer send/abort seams (B1-1123 PR-1): the plan's DEFINITION OF
// DONE, and impossible to satisfy by renaming anything -- before this PR,
// send_fn/send_ctx were module-global singletons
// (bb_data_http_set_send_fn() REPLACES, never appends), so a second
// consumer's send_fn silently evicted the first. Two independently-
// registered clients, each carrying its own send_fn/send_ctx set at acquire
// time via bb_data_http_client_cfg_t, must BOTH receive frames from a
// SINGLE sweep_step() call.
// ---------------------------------------------------------------------------

typedef struct {
    int  count;
    char last_keys[4][BB_DATA_HTTP_KEY_MAX];
} capture_calls_t;

static bb_err_t capture_a(const char *key, const bb_data_http_client_t *client,
                          const void *bytes, size_t len, void *ctx)
{
    (void)client;
    (void)bytes;
    (void)len;
    capture_calls_t *calls = (capture_calls_t *)ctx;
    if (calls->count < 4) {
        strncpy(calls->last_keys[calls->count], key, sizeof(calls->last_keys[0]) - 1);
        calls->last_keys[calls->count][sizeof(calls->last_keys[0]) - 1] = '\0';
    }
    calls->count++;
    return BB_OK;
}

// Distinct function (not a capture_a alias) so each client's cfg.send_fn is
// a genuinely different function pointer, not just a different ctx --
// mirrors two unrelated real consumers (e.g. a future HTTP client and an
// MQTT client) rather than one consumer acquired twice.
static bb_err_t capture_b(const char *key, const bb_data_http_client_t *client,
                          const void *bytes, size_t len, void *ctx)
{
    return capture_a(key, client, bytes, len, ctx);
}

void test_bb_data_http_sweep_step_delivers_to_multiple_independent_send_fn_consumers(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k2",.topic="topic.b"});

    capture_calls_t a_calls = {0}, b_calls = {0};
    bb_data_http_client_t *ca = NULL, *cb = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(
        &(bb_data_http_client_cfg_t){.send_fn = capture_a, .send_ctx = &a_calls}, &ca));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(
        &(bb_data_http_client_cfg_t){.send_fn = capture_b, .send_ctx = &b_calls}, &cb));

    fake_gen_bump("k1");
    fake_gen_bump("k2");
    bb_data_http_sweep_step();  // ONE sweep_step() call for BOTH clients

    // A and B each independently recorded BOTH frames (attach order: k1 then
    // k2), with the correct `key` argument -- impossible before this PR (one
    // global send_fn slot, the second acquire's set_send_fn() call would
    // have evicted the first).
    TEST_ASSERT_EQUAL_INT(2, a_calls.count);
    TEST_ASSERT_EQUAL_STRING("k1", a_calls.last_keys[0]);
    TEST_ASSERT_EQUAL_STRING("k2", a_calls.last_keys[1]);
    TEST_ASSERT_EQUAL_INT(2, b_calls.count);
    TEST_ASSERT_EQUAL_STRING("k1", b_calls.last_keys[0]);
    TEST_ASSERT_EQUAL_STRING("k2", b_calls.last_keys[1]);
}

// Regression proof, a separate test (not the same client pool as the
// two-consumer test above -- CONFIG_BB_DATA_HTTP_MAX_CLIENTS defaults to 2,
// too small to also carry a third client in the same test): a client
// acquired with NO per-client send_fn override must still route through the
// module-wide default installed via bb_data_http_set_send_fn() -- every
// existing HTTP call site relies on exactly this fallback, so this PR must
// not disturb it.
void test_bb_data_http_sweep_step_client_with_no_send_fn_override_uses_module_wide_default(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_host_install_send();
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="k1",.topic="topic.a"});
    fake_gen_set("k1", 1);

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c));

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_host_frame_count());
}

// B1-1448 (epic B1-1123 acceptance test): the epic's stated goal is that a
// push-only, non-socket consumer -- the prototypical example being a future
// MQTT publisher -- can register with bb_data_http carrying NO socket-shaped
// field and NO sentinel, because neither exists on bb_data_http_client_cfg_t
// anymore (the old fd/is_ws fields, and the fd sentinel that used to gate
// them, are gone). The designated initializer below names ONLY the fields an
// MQTT-shaped consumer actually needs -- subscribe_mask (EVENT-only: an
// MQTT publisher has no socket to poll-render STATE onto) plus its own
// send_fn/send_ctx (B1-1123 PR-1's per-consumer seam) -- and still acquires
// and delivers correctly, proving the acquire/deliver path never implicitly
// depended on a socket-shaped field existing.
void test_bb_data_http_mqtt_shaped_client_acquires_and_delivers_with_no_socket_field(void)
{
    reset_all();
    bb_data_http_init(NULL);
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    bb_data_http_attach(&(bb_data_http_attach_cfg_t){.key="ev1",.topic="topic.a",.kind=BB_DATA_HTTP_EVENT});

    capture_calls_t mqtt_calls = {0};
    bb_data_http_client_t *mqtt = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(
        &(bb_data_http_client_cfg_t){
            .subscribe_mask = BB_DATA_HTTP_SUBSCRIBE_EVENT,
            .send_fn        = capture_a,
            .send_ctx       = &mqtt_calls,
        }, &mqtt));

    // Push-only: no STATE bookkeeping block at all -- an MQTT publisher has
    // no socket to poll-render onto in the first place.
    TEST_ASSERT_TRUE(bb_data_http_client_poll_is_null_for_test(mqtt));
    TEST_ASSERT_FALSE(bb_data_http_client_push_is_null_for_test(mqtt));

    fake_gen_bump("ev1");
    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_INT(1, mqtt_calls.count);
    TEST_ASSERT_EQUAL_STRING("ev1", mqtt_calls.last_keys[0]);
}

// ---------------------------------------------------------------------------
// GET /api/events route descriptor (B1-1215) -- production fix for
// /api/events' absence from /api/openapi.json. bb_data_http_events_route()
// is the exact static descriptor bb_data_http_espidf_routes_init() registers
// on-device (see bb_data_http_espidf.c); these tests exercise the same
// object host-side rather than a mirrored fixture.
// ---------------------------------------------------------------------------

void test_bb_data_http_events_route_shape(void)
{
    const bb_route_t *r = bb_data_http_events_route();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(BB_HTTP_GET, r->method);
    TEST_ASSERT_EQUAL_STRING("/api/events", r->path);
    // schema-only descriptor: the real handler is wired separately (ESP-IDF
    // composition helper pairs it with events_get_handler via
    // bb_http_register_route()); the descriptor itself only carries OpenAPI
    // metadata, so .handler stays NULL here.
    TEST_ASSERT_NULL(r->handler);
    TEST_ASSERT_NOT_NULL(r->responses);
    TEST_ASSERT_EQUAL(200, r->responses[0].status);
    TEST_ASSERT_EQUAL_STRING("text/event-stream", r->responses[0].content_type);
    // Multiplexed stream over every attached topic -- no single response
    // schema; bb_openapi's oneOf synthesis fills this from registered SSE
    // topic schemas instead (see build_sse_oneof_fragment()).
    TEST_ASSERT_NULL(r->responses[0].schema);
}

void test_bb_data_http_events_route_registers_into_registry(void)
{
    bb_http_route_registry_clear();
    TEST_ASSERT_EQUAL_size_t(0, bb_http_route_registry_count());

    TEST_ASSERT_EQUAL(BB_OK,
        bb_http_register_route_descriptor_only(bb_data_http_events_route()));
    TEST_ASSERT_EQUAL_size_t(1, bb_http_route_registry_count());
    TEST_ASSERT_TRUE(bb_http_uri_is_registered("/api/events"));
}

void test_bb_data_http_events_route_appears_in_openapi_doc(void)
{
    bb_http_route_registry_clear();
    bb_http_register_route_descriptor_only(bb_data_http_events_route());
    // emit_operation() only emits a "content" block for a schema==NULL SSE
    // response when at least one SSE topic schema is registered (see
    // build_sse_oneof_fragment()'s gate) -- register one so the 200 entry's
    // content/text/event-stream media type is actually present to assert on.
    static const char k_schema[] =
        "{\"title\":\"LogEvent\",\"x-sse-topic\":\"log\",\"type\":\"object\","
        "\"properties\":{\"msg\":{\"type\":\"string\"}},\"required\":[\"msg\"]}";
    bb_openapi_register_schema("LogEvent", k_schema, "log");

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    TEST_ASSERT_NOT_NULL(paths);
    cJSON *events_pi = cJSON_GetObjectItemCaseSensitive(paths, "/api/events");
    TEST_ASSERT_NOT_NULL(events_pi);
    cJSON *get_op = cJSON_GetObjectItemCaseSensitive(events_pi, "get");
    TEST_ASSERT_NOT_NULL(get_op);
    cJSON *responses = cJSON_GetObjectItemCaseSensitive(get_op, "responses");
    cJSON *r200 = cJSON_GetObjectItemCaseSensitive(responses, "200");
    TEST_ASSERT_NOT_NULL(r200);
    cJSON *content = cJSON_GetObjectItemCaseSensitive(r200, "content");
    TEST_ASSERT_NOT_NULL(content);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(content, "text/event-stream"));

    test_openapi_capture_free(&r);
}

// Registering the production descriptor alongside a real SSE topic schema
// gives build_sse_oneof_fragment() a production trigger for the first time
// (B1-1215) -- previously nothing in the tree ever registered a route with
// content_type=="text/event-stream" and schema==NULL, so this synthesis path
// was fixture-only (test_sse_schema_fidelity.c's s_sse_route). Confirms the
// oneOf actually fires for the real /api/events descriptor.
void test_bb_data_http_events_route_oneof_synthesized_for_registered_topic(void)
{
    bb_http_route_registry_clear();
    bb_http_register_route_descriptor_only(bb_data_http_events_route());
    static const char k_schema[] =
        "{\"title\":\"LogEvent\",\"x-sse-topic\":\"log\",\"type\":\"object\","
        "\"properties\":{\"msg\":{\"type\":\"string\"}},\"required\":[\"msg\"]}";
    bb_openapi_register_schema("LogEvent", k_schema, "log");

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"oneOf\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/LogEvent\""));

    test_openapi_capture_free(&r);
}

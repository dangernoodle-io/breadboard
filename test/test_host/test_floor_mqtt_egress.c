// Host tests for floor's MQTT egress consumer seam (B1-1126 PR-1 of 2).
// examples/floor is an example, not a component, so it is not part of the
// native scaffold's component graph and cannot be reached the normal way --
// this test #includes floor_mqtt_egress.c directly (a relative path into
// examples/floor/main/), the SAME translation unit floor_app.c compiles,
// not a hand-mirrored copy. B1-1483: this seam is currently NOT composed
// by floor_app.c (see floor_mqtt_egress.h's doc for why + the precondition
// for re-composing it) -- these tests still exercise the seam's functions
// directly and remain the load-bearing coverage for it.
#include "unity.h"
#include "../../examples/floor/main/floor_mqtt_egress.c"

#include "bb_mqtt_client.h"

#include "../../components/bb_data_http/src/bb_data_http_internal.h"

#include <inttypes.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fixtures: a fake generation store + a fake render_fn, mirroring
// test_bb_data_http.c's own fixtures -- this is a separate translation unit,
// so these are this file's own copies, not shared symbols.
// ---------------------------------------------------------------------------

#define FAKE_GEN_MAX 4

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

static void fake_gen_bump(const char *key)
{
    for (size_t i = 0; i < s_fake_gen_count; i++) {
        if (strcmp(s_fake_gens[i].key, key) == 0) {
            s_fake_gens[i].gen++;
            return;
        }
    }
    s_fake_gens[s_fake_gen_count].key = key;
    s_fake_gens[s_fake_gen_count].gen = 1;
    s_fake_gen_count++;
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

static bb_err_t fake_render_fn(const char *key, char *buf, size_t cap, size_t *out_len, void *ctx)
{
    (void)ctx;
    uint32_t gen = 0;
    fake_generation_fn(key, &gen, NULL);
    int n = snprintf(buf, cap, "{\"key\":\"%s\",\"gen\":%" PRIu32 "}", key, gen);
    if (n < 0 || (size_t)n >= cap) return BB_ERR_NO_SPACE;  // LCOV_EXCL_LINE -- fixture buffers are always large enough
    *out_len = (size_t)n;
    return BB_OK;
}

// Counts calls to floor_mqtt_egress_send_fn() -- a thin wrapper, not a
// mirror: it always delegates to the real function under test, and exists
// only because none of bb_mqtt_client's host hooks count REJECTED (fatal)
// enqueue attempts (only accepted ones), so this is the only way to prove
// bb_data_http's flush loop stopped after exactly one send_fn call rather
// than attempting a second queued frame.
static int s_send_fn_calls;
static bb_err_t counting_send_fn(const char *key, const bb_data_http_client_t *client,
                                  const void *bytes, size_t len, void *ctx)
{
    s_send_fn_calls++;
    return floor_mqtt_egress_send_fn(key, client, bytes, len, ctx);
}

static void reset_all(void)
{
    bb_data_http_reset_for_test();
    bb_mqtt_client_default_set(NULL);
    fake_gen_reset();
    s_send_fn_calls = 0;
}

// ---------------------------------------------------------------------------
// floor_mqtt_send_rc_to_bb -- the load-bearing pure translation. Every input
// in the documented matrix (floor_mqtt_egress.h), no MQTT or bb_data_http
// involvement at all.
// ---------------------------------------------------------------------------

void test_floor_mqtt_send_rc_to_bb_ok_maps_to_ok(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, floor_mqtt_send_rc_to_bb(BB_OK));
}

// Mutation evidence: mapping this case to fatal (i.e. `return enqueue_rc`
// instead of `return BB_ERR_TIMEOUT`) makes this FAIL -- the single most
// damaging misclassification available here (outbox-full is the routine,
// expected-under-load case, and bb_data_http's two-class contract treats
// anything non-BB_ERR_TIMEOUT as unretryable/FATAL).
void test_floor_mqtt_send_rc_to_bb_no_space_maps_to_timeout(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, floor_mqtt_send_rc_to_bb(BB_ERR_NO_SPACE));
}

// Mutation evidence: mapping this case to fatal makes this FAIL.
void test_floor_mqtt_send_rc_to_bb_invalid_state_maps_to_timeout(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, floor_mqtt_send_rc_to_bb(BB_ERR_INVALID_STATE));
}

// Mutation evidence: mapping this case to BB_ERR_TIMEOUT instead of passing
// it through makes this FAIL -- a genuine caller-bug reject (oversized
// topic, bad args) would then be silently retried forever instead of
// surfacing as the fatal (if unbounded-retry-at-head) condition it is.
void test_floor_mqtt_send_rc_to_bb_validation_passes_through_fatal(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, floor_mqtt_send_rc_to_bb(BB_ERR_VALIDATION));
}

// Any other non-BB_OK/non-retriable code also falls to the fatal default --
// proves the switch's default arm, not just the two named retriable cases.
void test_floor_mqtt_send_rc_to_bb_other_code_passes_through_fatal(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, floor_mqtt_send_rc_to_bb(BB_ERR_INVALID_ARG));
}

// ---------------------------------------------------------------------------
// floor_mqtt_egress_send_fn -- unit tests against bb_mqtt_client's host stub
// directly (no bb_data_http sweep involved).
// ---------------------------------------------------------------------------

// NULL handle -> retriable, no crash, and (implicitly, since this returns
// before ever calling bb_mqtt_client_enqueue()) no attempt to enqueue on a
// NULL handle. Mutation evidence: removing the NULL check and calling
// bb_mqtt_client_enqueue(NULL, ...) unconditionally makes this FAIL -- the
// host stub's own NULL-handle guard returns BB_ERR_INVALID_ARG, which this
// function's default (fatal) branch would then return instead of
// BB_ERR_TIMEOUT.
void test_floor_mqtt_egress_send_fn_null_default_client_returns_timeout(void)
{
    reset_all();
    bb_mqtt_client_default_set(NULL);
    bb_err_t rc = floor_mqtt_egress_send_fn("log", NULL, "hi", 2, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, rc);
}

// Builds "<prefix>/<key>" and hands off via enqueue (never publish) --
// proves the topic convention end to end against a live (host-stub) handle.
void test_floor_mqtt_egress_send_fn_builds_prefixed_topic_and_enqueues(void)
{
    reset_all();
    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);

    bb_err_t rc = floor_mqtt_egress_send_fn("log", NULL, "hi", 2, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    const bb_mqtt_client_host_pub_t *pub = bb_mqtt_client_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(pub);
    TEST_ASSERT_EQUAL_STRING("floor/log", pub->topic);
    TEST_ASSERT_EQUAL_STRING("hi", pub->payload);

    bb_mqtt_client_destroy(h);
}

// esp_mqtt_client_enqueue()'s outbox-full outcome (msg_id==-2 ->
// BB_ERR_NO_SPACE) translates to BB_ERR_TIMEOUT.
void test_floor_mqtt_egress_send_fn_outbox_full_returns_timeout(void)
{
    reset_all();
    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);
    bb_mqtt_client_host_set_enqueue_msg_id(h, -2);

    bb_err_t rc = floor_mqtt_egress_send_fn("log", NULL, "hi", 2, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, rc);

    bb_mqtt_client_destroy(h);
}

// esp_mqtt_client_enqueue()'s generic-reject outcome (msg_id==-1 ->
// BB_ERR_VALIDATION) passes through unchanged (fatal).
void test_floor_mqtt_egress_send_fn_validation_reject_returns_fatal(void)
{
    reset_all();
    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);
    bb_mqtt_client_host_set_enqueue_msg_id(h, -1);

    bb_err_t rc = floor_mqtt_egress_send_fn("log", NULL, "hi", 2, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);

    bb_mqtt_client_destroy(h);
}

// Oversized key -> BB_ERR_VALIDATION (LOW review fix). Provably unreachable
// through the real dispatch path (bb_data_http_attach() rejects any key >=
// BB_DATA_HTTP_KEY_MAX before floor_mqtt_egress_send_fn() would ever see it
// -- see FLOOR_MQTT_EGRESS_TOPIC_MAX's doc comment) -- this is a DIRECT call
// exercising the guard itself, not a reachable-through-dispatch scenario.
// NOT mutation-sensitive to a buffer *shrink* on its own: the truncation
// check is a plain `>=` threshold, so shrinking the buffer can only WIDEN
// the set of keys that truncate, never narrow it -- a key this far past the
// threshold (60 vs the 49-char minimum that already truncates today) keeps
// truncating (and this test keeps passing) no matter how much smaller the
// buffer gets. See the companion boundary test right below for the one that
// actually flips on a 1-byte shrink.
void test_floor_mqtt_egress_send_fn_oversized_key_returns_validation(void)
{
    reset_all();
    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);

    char oversized_key[61];
    memset(oversized_key, 'k', sizeof(oversized_key) - 1);
    oversized_key[sizeof(oversized_key) - 1] = '\0';

    bb_err_t rc = floor_mqtt_egress_send_fn(oversized_key, NULL, "hi", 2, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);

    bb_mqtt_client_destroy(h);
}

// Boundary test for the documented "one byte of slack" claim
// (FLOOR_MQTT_EGRESS_TOPIC_MAX's doc comment): a key of exactly
// BB_DATA_HTTP_KEY_MAX (48) characters -- itself already illegal for
// bb_data_http_attach() (which rejects length >= BB_DATA_HTTP_KEY_MAX), but
// still one character SHORT of the 49-char length that first triggers this
// buffer's own truncation guard -- must fit with zero truncation, proving
// the documented slack is real. THIS is the mutation-sensitive test: a
// key length hardcoded at the current invariant's exact edge, not derived
// from the macro, so shrinking FLOOR_MQTT_EGRESS_TOPIC_MAX by even 1 byte
// erases that slack and flips this key from "fits" to "truncates" --
// verified: reducing the macro by 1 turns this test's rc from BB_OK to
// BB_ERR_VALIDATION.
void test_floor_mqtt_egress_send_fn_key_at_slack_boundary_fits(void)
{
    reset_all();
    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);

    char boundary_key[BB_DATA_HTTP_KEY_MAX + 1];
    memset(boundary_key, 'k', BB_DATA_HTTP_KEY_MAX);
    boundary_key[BB_DATA_HTTP_KEY_MAX] = '\0';

    bb_err_t rc = floor_mqtt_egress_send_fn(boundary_key, NULL, "hi", 2, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    bb_mqtt_client_destroy(h);
}

// ---------------------------------------------------------------------------
// floor_mqtt_egress_should_acquire -- pure gate (HIGH review fix): must not
// acquire a durable bb_data_http slot when no MQTT client is configured
// (bb_mqtt_client_init_default() returns BB_OK even when MQTT is disabled
// in NVS, a no-op, no client created).
// ---------------------------------------------------------------------------

// Mutation evidence: inverting this (`== NULL`) or hard-coding true makes
// this FAIL.
void test_floor_mqtt_egress_should_acquire_false_when_no_client(void)
{
    TEST_ASSERT_FALSE(floor_mqtt_egress_should_acquire(NULL));
}

// Mutation evidence: hard-coding false makes this FAIL.
void test_floor_mqtt_egress_should_acquire_true_when_client_present(void)
{
    reset_all();
    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));

    TEST_ASSERT_TRUE(floor_mqtt_egress_should_acquire(h));

    bb_mqtt_client_destroy(h);
}

// ---------------------------------------------------------------------------
// floor_mqtt_egress_abort_fn -- unreachable from bb_data_http's own flush
// loop for a DURABLE client (see floor_mqtt_egress.c's doc on this
// function), so this test is this line's ONLY coverage: it exercises the
// function directly, standing in for the
// bb_data_http_client_request_release() path this consumer's cfg installs
// it for.
// ---------------------------------------------------------------------------

void test_floor_mqtt_egress_abort_fn_releases_client(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_init(NULL));
    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){0}, &c));
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_active_client_count());

    floor_mqtt_egress_abort_fn(c, NULL);

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_active_client_count());
}

// ---------------------------------------------------------------------------
// Integration: a REAL bb_data_http_sweep_step() driving a DURABLE client
// wired to floor_mqtt_egress_send_fn against bb_mqtt_client's host stub.
// ---------------------------------------------------------------------------

// Outbox-full -> client survives, queue retained, frame retried next sweep
// and delivered once space frees. send_fail_count_for_test()==1 after the
// first sweep is the mutation-sensitive assertion: it is only 1 (not 0) if
// the failure was classified retriable (BB_ERR_TIMEOUT), not fatal.
void test_floor_mqtt_egress_sweep_outbox_full_retries_then_delivers(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_init(NULL));
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_attach(&(bb_data_http_attach_cfg_t){
        .key = "log", .topic = "log" }));

    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);
    bb_mqtt_client_host_set_outbox_limit(h, 1);  // smaller than the rendered frame -> NO_SPACE

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){
        .topic_filter = "log", .send_fn = floor_mqtt_egress_send_fn,
        .lifetime = BB_DATA_HTTP_LIFETIME_DURABLE, .non_blocking = true }, &c));
    fake_gen_bump("log");  // dirties "log" for this sweep

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_active_client_count());        // survives
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_client_outbound_count_for_test(c));  // retained
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_send_fail_count_for_test(c));  // retriable, not fatal
    TEST_ASSERT_EQUAL(0, bb_mqtt_client_host_pub_count(h));               // nothing accepted yet

    bb_mqtt_client_host_set_outbox_limit(h, 0);  // free space (unbounded)

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(0, bb_data_http_client_outbound_count_for_test(c));  // delivered
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_send_fail_count_for_test(c));
    TEST_ASSERT_EQUAL(1, bb_mqtt_client_host_pub_count(h));
    const bb_mqtt_client_host_pub_t *pub = bb_mqtt_client_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(pub);
    TEST_ASSERT_EQUAL_STRING("floor/log", pub->topic);

    bb_mqtt_client_destroy(h);
}

// NULL handle -> retriable, no crash, client not torn down.
void test_floor_mqtt_egress_sweep_null_handle_retries_no_teardown(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_init(NULL));
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_attach(&(bb_data_http_attach_cfg_t){
        .key = "log", .topic = "log" }));

    bb_mqtt_client_default_set(NULL);  // no client this boot (MQTT disabled/suspended)

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){
        .topic_filter = "log", .send_fn = floor_mqtt_egress_send_fn,
        .lifetime = BB_DATA_HTTP_LIFETIME_DURABLE, .non_blocking = true }, &c));
    fake_gen_bump("log");

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_active_client_count());
    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_client_outbound_count_for_test(c));  // retained, retried later
    TEST_ASSERT_EQUAL_UINT32(1u, bb_data_http_client_send_fail_count_for_test(c));  // retriable
}

// Validation-reject -> DURABLE client survives, and the flush stops for
// this sweep (a second dirty key queued the same sweep is never attempted).
// s_send_fn_calls==1 (not 2) is the assertion that actually proves "flush
// stops" -- bb_mqtt_client's host stub has no counter for rejected (as
// opposed to accepted) enqueue attempts, so this test wraps
// floor_mqtt_egress_send_fn in a counting shim instead.
// send_fail_count_for_test()==0 is the mutation-sensitive companion
// assertion: it is only 0 (not 1) if the rejection was classified fatal,
// not retriable.
void test_floor_mqtt_egress_sweep_validation_reject_survives_and_stops_flush(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_init(NULL));
    bb_data_http_set_generation_fn(fake_generation_fn, NULL);
    bb_data_http_set_render_fn(fake_render_fn, NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_attach(&(bb_data_http_attach_cfg_t){
        .key = "log", .topic = "log" }));
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_attach(&(bb_data_http_attach_cfg_t){
        .key = "other", .topic = "other" }));

    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);
    bb_mqtt_client_host_set_enqueue_msg_id(h, -1);  // sticky generic reject

    bb_data_http_client_t *c = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_http_client_acquire(&(bb_data_http_client_cfg_t){
        .topic_filter = NULL, .send_fn = counting_send_fn,
        .lifetime = BB_DATA_HTTP_LIFETIME_DURABLE, .non_blocking = true }, &c));
    fake_gen_bump("log");
    fake_gen_bump("other");

    bb_data_http_sweep_step();

    TEST_ASSERT_EQUAL_UINT(1, bb_data_http_active_client_count());  // DURABLE survives
    TEST_ASSERT_EQUAL_INT(1, s_send_fn_calls);                     // flush stopped after 1 attempt
    TEST_ASSERT_EQUAL_UINT32(0u, bb_data_http_client_send_fail_count_for_test(c));  // fatal, not retriable
    TEST_ASSERT_EQUAL_UINT(2, bb_data_http_client_outbound_count_for_test(c));  // neither frame popped

    bb_mqtt_client_destroy(h);
}

// Host test suite for bb_cache's registration/update/delete/enumeration
// mechanism.
//
// B1-1119: bb_cache no longer ships a legacy bb_json serializer slot
// (bb_cache_serialize_fn / bb_cache_get_serialized() / bb_cache_serialize_into()
// are gone -- the same-serializer "fidelity" guarantee this file used to
// prove no longer applies; every read path is bb_cache_get_raw()/
// bb_cache_snapshot() now). This file's name is kept for history/git-blame
// continuity; its content is the general bb_cache mechanism suite.

#include "unity.h"
#include "bb_cache.h"
#include "bb_clock.h"
#include "bb_ota_check_wire.h"
#include "bb_mem_test.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>

// ---------------------------------------------------------------------------
// Test reset hook — declared in bb_cache_espidf.c (BB_CACHE_TESTING)
// ---------------------------------------------------------------------------
void bb_cache_reset_for_test(void);

// Test helper: mirrors the pre-config-struct bb_cache_register() shape for
// tests that don't care about flags (B1-1045: BB_CACHE_FLAG_SSE is gone --
// SSE/broadcast delivery is a bb_data/bb_data_http composition-root concern
// now, not bb_cache's).
static bb_err_t reg(const char *key, const void *(*snapshot)(void), size_t snap_size)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = snapshot,
        .snap_size = snap_size,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    return bb_cache_register(&cfg);
}

// ---------------------------------------------------------------------------
// Synthetic test topic definition
// ---------------------------------------------------------------------------

typedef struct {
    int   value;
    bool  flag;
    float ratio;
} synth_snap_t;

// ---------------------------------------------------------------------------
// setUp / tearDown
// ---------------------------------------------------------------------------

static void reset_all(void)
{
    bb_cache_reset_for_test();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Verify register is idempotent (second call is a no-op, no duplicate entries).
void test_bb_cache_register_idempotent(void)
{
    reset_all();
    bb_err_t err;
    err = reg("test.synth", NULL, sizeof(synth_snap_t));
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    err = reg("test.synth", NULL, sizeof(synth_snap_t));
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

// Verify update mutates the owned struct and is reflected in a subsequent
// bb_cache_get_raw().
void test_bb_cache_update_reflected_in_get_raw(void)
{
    reset_all();
    reg("test.synth", NULL, sizeof(synth_snap_t));

    synth_snap_t s1 = {.value = 1, .flag = false, .ratio = 0.1f};
    bb_cache_update(&(bb_cache_update_t){ .key = "test.synth", .snap = &s1 });
    synth_snap_t out1 = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.synth", &out1, sizeof(out1)));
    TEST_ASSERT_EQUAL_INT(1, out1.value);

    synth_snap_t s2 = {.value = 99, .flag = true, .ratio = 3.14f};
    bb_cache_update(&(bb_cache_update_t){ .key = "test.synth", .snap = &s2 });
    synth_snap_t out2 = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.synth", &out2, sizeof(out2)));
    TEST_ASSERT_EQUAL_INT(99, out2.value);

    // Re-read after the second update must still match.
    synth_snap_t out3 = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.synth", &out3, sizeof(out3)));
    TEST_ASSERT_EQUAL_INT(out2.value, out3.value);
}

// Verify update on an unknown topic returns BB_ERR_NOT_FOUND.
void test_bb_cache_update_unknown_topic(void)
{
    reset_all();
    synth_snap_t s = {0};
    bb_err_t err = bb_cache_update(&(bb_cache_update_t){ .key = "no.such.topic", .snap = &s });
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, err);
}

// Verify registry full returns BB_ERR_NO_SPACE.
// bb_cache_register() copies the key, so a reused stack buffer is fine.
void test_bb_cache_registry_full(void)
{
    reset_all();
    char key_buf[32];

    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        snprintf(key_buf, sizeof(key_buf), "test.fill.%d", i);
        bb_err_t err = reg(key_buf, NULL, sizeof(synth_snap_t));
        TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, err, "fill should succeed");
    }
    // One more must fail
    bb_err_t err = reg("test.fill.overflow", NULL, sizeof(synth_snap_t));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
}

// Pins BB_OTA_CHECK_TOPIC's string VALUE to "update.available" so a future
// bb_ota_check refactor cannot silently drift the SSE/cache topic name (the
// macro is free to be renamed; the wire value is the public contract).
void test_bb_ota_check_topic_value_is_update_available(void)
{
    TEST_ASSERT_EQUAL_STRING("update.available", BB_OTA_CHECK_TOPIC);
}

// ---------------------------------------------------------------------------
// Keyed enumeration + get_raw tests
// ---------------------------------------------------------------------------

typedef struct {
    bool seen_a, seen_b, seen_c;
    int  seen_count;
} foreach_visit_ctx_t;

static void foreach_visit_cb(const char *topic, void *ctx)
{
    foreach_visit_ctx_t *c = (foreach_visit_ctx_t *)ctx;
    c->seen_count++;
    if (strcmp(topic, "test.enum.a") == 0) c->seen_a = true;
    if (strcmp(topic, "test.enum.b") == 0) c->seen_b = true;
    if (strcmp(topic, "test.enum.c") == 0) c->seen_c = true;
}

// Verify bb_cache_count/foreach/key_at over a populated registry.
void test_bb_cache_count_and_foreach_visit_all_registered_topics(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_UINT(0, bb_cache_count());

    synth_snap_t s = {.value = 1, .flag = false, .ratio = 0.0f};
    reg("test.enum.a", NULL, sizeof(synth_snap_t));
    bb_cache_update(&(bb_cache_update_t){ .key = "test.enum.a", .snap = &s });
    reg("test.enum.b", NULL, sizeof(synth_snap_t));
    bb_cache_update(&(bb_cache_update_t){ .key = "test.enum.b", .snap = &s });
    reg("test.enum.c", NULL, sizeof(synth_snap_t));
    bb_cache_update(&(bb_cache_update_t){ .key = "test.enum.c", .snap = &s });

    TEST_ASSERT_EQUAL_UINT(3, bb_cache_count());

    foreach_visit_ctx_t ctx = {0};
    bb_err_t err = bb_cache_foreach(foreach_visit_cb, &ctx);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(3, ctx.seen_count);
    TEST_ASSERT_TRUE(ctx.seen_a);
    TEST_ASSERT_TRUE(ctx.seen_b);
    TEST_ASSERT_TRUE(ctx.seen_c);
}

// Verify bb_cache_key_at returns the registered topics and handles free/OOB
// slots correctly.
void test_bb_cache_key_at_free_and_oob_slots(void)
{
    reset_all();
    reg("test.enum.only", NULL, sizeof(synth_snap_t));

    bool found_registered = false;
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        char buf[BB_CACHE_KEY_MAX];
        memset(buf, 0xAA, sizeof(buf)); // sentinel to detect no-write
        bb_err_t err = bb_cache_key_at((size_t)i, buf, sizeof(buf));
        TEST_ASSERT_EQUAL_INT(BB_OK, err);
        if (buf[0] != '\0') {
            TEST_ASSERT_EQUAL_STRING("test.enum.only", buf);
            found_registered = true;
        }
    }
    TEST_ASSERT_TRUE(found_registered);

    // OOB index
    char out[BB_CACHE_KEY_MAX];
    bb_err_t err = bb_cache_key_at((size_t)BB_CACHE_MAX_TOPICS, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, err);
}

// Verify bb_cache_key_at rejects a NULL buf or cap == 0.
void test_bb_cache_key_at_null_out_returns_invalid_arg(void)
{
    reset_all();
    char buf[BB_CACHE_KEY_MAX];
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_key_at(0, NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_key_at(0, buf, 0));
}

// Verify bb_cache_key_at refuses (without copying) when cap is too small to
// hold the key plus its NUL terminator.
void test_bb_cache_key_at_undersized_buf_returns_no_space(void)
{
    reset_all();
    reg("test.enum.longkey", NULL, sizeof(synth_snap_t));

    bool saw_no_space = false;
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        char tiny[4];
        memset(tiny, 0xBB, sizeof(tiny));
        bb_err_t err = bb_cache_key_at((size_t)i, tiny, sizeof(tiny));
        if (err == BB_ERR_NO_SPACE) {
            saw_no_space = true;
            // Buffer must be untouched on refusal.
            TEST_ASSERT_EQUAL_UINT8(0xBB, (uint8_t)tiny[0]);
        } else {
            TEST_ASSERT_EQUAL_INT(BB_OK, err);
        }
    }
    TEST_ASSERT_TRUE(saw_no_space);
}

// Verify bb_cache_foreach rejects a NULL callback.
void test_bb_cache_foreach_null_cb_returns_invalid_arg(void)
{
    reset_all();
    bb_err_t err = bb_cache_foreach(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

// Verify bb_cache_get_raw round-trips an owned-mode struct and refuses
// when cap < size (does not copy on NO_SPACE).
void test_bb_cache_get_raw_round_trip_and_refuses_undersized(void)
{
    reset_all();
    reg("test.raw", NULL, sizeof(synth_snap_t));

    synth_snap_t in = {.value = 7, .flag = true, .ratio = 2.5f};
    bb_err_t err = bb_cache_update(&(bb_cache_update_t){ .key = "test.raw", .snap = &in });
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    // Exact-size round-trip: cap == size succeeds.
    synth_snap_t out = {0};
    err = bb_cache_get_raw("test.raw", &out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(in.value, out.value);
    TEST_ASSERT_EQUAL(in.flag, out.flag);
    TEST_ASSERT_EQUAL_FLOAT(in.ratio, out.ratio);

    // Undersized buffer: cap < size refuses without copying.
    unsigned char partial[sizeof(int)];
    memset(partial, 0xAA, sizeof(partial));  // pre-fill with sentinel
    err = bb_cache_get_raw("test.raw", partial, sizeof(int));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
    // Buffer should be untouched (still sentinel).
    TEST_ASSERT_EQUAL_UINT8(0xAA, partial[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAA, partial[1]);
    TEST_ASSERT_EQUAL_UINT8(0xAA, partial[2]);
    TEST_ASSERT_EQUAL_UINT8(0xAA, partial[3]);

    // Oversized buffer: cap > size succeeds, copies full size.
    unsigned char oversized[sizeof(synth_snap_t) + 16];
    memset(oversized, 0xBB, sizeof(oversized));
    err = bb_cache_get_raw("test.raw", oversized, sizeof(oversized));
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    synth_snap_t oversized_out;
    memcpy(&oversized_out, oversized, sizeof(oversized_out));
    TEST_ASSERT_EQUAL_INT(in.value, oversized_out.value);
    // Bytes after the struct should still be untouched.
    TEST_ASSERT_EQUAL_UINT8(0xBB, oversized[sizeof(synth_snap_t)]);
}

static synth_snap_t s_getter_backing = {.value = 5, .flag = true, .ratio = 1.0f};
static const void *get_raw_test_getter(void) { return &s_getter_backing; }

// Verify bb_cache_get_raw on a getter-mode topic returns BB_ERR_INVALID_STATE.
void test_bb_cache_get_raw_getter_mode_returns_invalid_state(void)
{
    reset_all();
    reg("test.raw.getter", get_raw_test_getter, 0);

    unsigned char buf[sizeof(synth_snap_t)];
    bb_err_t err = bb_cache_get_raw("test.raw.getter", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, err);
}

// Verify bb_cache_get_raw on an absent topic returns BB_ERR_NOT_FOUND.
void test_bb_cache_get_raw_absent_topic_returns_not_found(void)
{
    reset_all();
    unsigned char buf[16];
    bb_err_t err = bb_cache_get_raw("no.such.raw.topic", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, err);
}

// Verify bb_cache_get_raw rejects null args / cap == 0.
void test_bb_cache_get_raw_null_args_return_invalid_arg(void)
{
    reset_all();
    reg("test.raw.args", NULL, sizeof(synth_snap_t));
    synth_snap_t s = {0};
    bb_cache_update(&(bb_cache_update_t){ .key = "test.raw.args", .snap = &s });

    unsigned char buf[sizeof(synth_snap_t)];
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_get_raw(NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_get_raw("test.raw.args", NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_get_raw("test.raw.args", buf, 0));
}

// ---------------------------------------------------------------------------
// B1-568: find_entry() lock safety under concurrent registration
// ---------------------------------------------------------------------------
//
// Regression test for a torn-read race: bb_cache_update/get_raw used to scan
// s_entries[] via a raw, unlocked find_entry() while bb_cache_register()
// concurrently wrote new slot fields under s_reg_lock. This test hammers a
// stable, already-registered topic with lookups (bb_cache_get_raw) from one
// thread while a second thread concurrently registers a burst of new topics
// -- exercising exactly the interleaving the fix (find_entry_locked) guards
// against.
//
// NOT A HARD REGRESSION GUARD without a race detector: this test asserts
// observable outcomes (lookups never fail, content is always well-formed,
// final registry state is correct) but cannot deterministically prove the
// absence of a torn read on a given run -- a data race is schedule-dependent
// and this plain host build has no ASan/TSan instrumentation to catch one
// even if it occurred. A sanitizer-enabled native test variant is tracked as
// a follow-up; until then, this test's value is in exercising the exact
// interleaving shape the fix targets, not in guaranteeing detection.

// Kept well below BB_CACHE_MAX_TOPICS (default 32) -- +1 for the stable
// entry registered outside the burst.
#define B1_568_BURST_TOPICS 20

// Unity's TEST_ASSERT_* macros longjmp on failure and are not safe to call
// from any thread but the one running the test runner -- worker threads
// record results into this shared struct instead; all assertions happen
// back on the main test thread after both threads are joined.
typedef struct {
    int register_fail_count;
    int lookup_fail_count;
    int lookup_bad_content_count;
} b1_568_result_t;

// bb_cache_register() copies the key it is given, so this static storage is
// not required for pointer-lifetime reasons -- kept as static (rather than a
// per-thread stack buffer) purely so both worker threads can format their
// names once, up front, before the concurrent register/lookup burst begins.
static char s_b1_568_names[B1_568_BURST_TOPICS][32];

static void *b1_568_writer_fn(void *arg)
{
    b1_568_result_t *res = (b1_568_result_t *)arg;
    for (int i = 0; i < B1_568_BURST_TOPICS; i++) {
        bb_err_t err = reg(s_b1_568_names[i], NULL, sizeof(synth_snap_t));
        if (err != BB_OK) {
            res->register_fail_count++;
            continue;
        }
        synth_snap_t s = {.value = i, .flag = (i % 2 == 0), .ratio = (float)i};
        bb_cache_update(&(bb_cache_update_t){ .key = s_b1_568_names[i], .snap = &s });
    }
    return NULL;
}

static void *b1_568_reader_fn(void *arg)
{
    b1_568_result_t *res = (b1_568_result_t *)arg;
    synth_snap_t out;
    for (int i = 0; i < B1_568_BURST_TOPICS; i++) {
        bb_err_t err = bb_cache_get_raw("test.b1568.stable", &out, sizeof(out));
        if (err != BB_OK) {
            res->lookup_fail_count++;
            continue;
        }
        if (out.value != 42) {
            res->lookup_bad_content_count++;
        }
    }
    return NULL;
}

void test_bb_cache_find_entry_locked_survives_concurrent_registration(void)
{
    reset_all();

    // Pre-existing stable entry, looked up by the reader thread throughout
    // the writer thread's registration burst.
    bb_err_t err = reg("test.b1568.stable", NULL, sizeof(synth_snap_t));
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    synth_snap_t stable = {.value = 42, .flag = true, .ratio = 1.0f};
    err = bb_cache_update(&(bb_cache_update_t){ .key = "test.b1568.stable", .snap = &stable });
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    for (int i = 0; i < B1_568_BURST_TOPICS; i++) {
        snprintf(s_b1_568_names[i], sizeof(s_b1_568_names[i]), "test.b1568.%d", i);
    }

    b1_568_result_t writer_res = {0};
    b1_568_result_t reader_res = {0};

    pthread_t writer_tid, reader_tid;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&writer_tid, NULL, b1_568_writer_fn, &writer_res));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&reader_tid, NULL, b1_568_reader_fn, &reader_res));

    pthread_join(writer_tid, NULL);
    pthread_join(reader_tid, NULL);

    TEST_ASSERT_EQUAL_INT(0, writer_res.register_fail_count);
    TEST_ASSERT_EQUAL_INT(0, reader_res.lookup_fail_count);
    TEST_ASSERT_EQUAL_INT(0, reader_res.lookup_bad_content_count);

    // stable + burst topics all present.
    TEST_ASSERT_EQUAL_UINT((unsigned)(1 + B1_568_BURST_TOPICS), bb_cache_count());
}

// ---------------------------------------------------------------------------
// B1-576: config-struct API + key-copy + CONFIG_BB_CACHE_KEY_MAX
// ---------------------------------------------------------------------------

// Verify the bb_cache_config_t struct form registers and round-trips exactly
// like the reg() helper's equivalent positional call.
void test_bb_cache_register_config_struct_basic(void)
{
    reset_all();
    bb_cache_config_t cfg = {
        .key       = "test.cfg.basic",
        .snapshot  = NULL,
        .snap_size = sizeof(synth_snap_t),
        .flags     = BB_CACHE_FLAG_NONE,
    };
    bb_err_t err = bb_cache_register(&cfg);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    synth_snap_t s = {.value = 5, .flag = true, .ratio = 1.5f};
    err = bb_cache_update(&(bb_cache_update_t){ .key = "test.cfg.basic", .snap = &s });
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    synth_snap_t out = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.cfg.basic", &out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(5, out.value);
}

// Verify bb_cache_register rejects a NULL cfg and NULL cfg->key.
void test_bb_cache_register_config_struct_null_args(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_register(NULL));

    bb_cache_config_t no_key = {
        .key = NULL, .snapshot = NULL, .snap_size = sizeof(synth_snap_t),
        .flags = BB_CACHE_FLAG_NONE,
    };
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_register(&no_key));
}

// Key-copy UAF test: the caller's key buffer is freed/overwritten immediately
// after bb_cache_register() returns. A later lookup by the ORIGINAL key value
// must still succeed, proving bb_cache copied the key rather than storing the
// caller's pointer (the pre-B1-576 footgun).
void test_bb_cache_register_key_is_copied_no_uaf(void)
{
    reset_all();

    char *key = (char *)malloc(32);
    TEST_ASSERT_NOT_NULL(key);
    strcpy(key, "test.uaf.key");

    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = NULL,
        .snap_size = sizeof(synth_snap_t),
        .flags     = BB_CACHE_FLAG_NONE,
    };
    bb_err_t err = bb_cache_register(&cfg);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    // Free the caller's key buffer immediately — bb_cache must have copied it.
    memset(key, 0xAA, 32);
    free(key);
    key = NULL;

    synth_snap_t s = {.value = 9, .flag = false, .ratio = 0.0f};
    err = bb_cache_update(&(bb_cache_update_t){ .key = "test.uaf.key", .snap = &s });
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, err, "lookup by original key value must survive freeing the caller's buffer");

    synth_snap_t out = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.uaf.key", &out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(9, out.value);
}

// Over-length key rejected with BB_ERR_INVALID_ARG, never silently truncated.
void test_bb_cache_register_overlength_key_rejected(void)
{
    reset_all();

    char long_key[BB_CACHE_KEY_MAX + 16];
    memset(long_key, 'k', sizeof(long_key) - 1);
    long_key[sizeof(long_key) - 1] = '\0';
    TEST_ASSERT_EQUAL_UINT(sizeof(long_key) - 1, strlen(long_key));
    TEST_ASSERT_TRUE(strlen(long_key) >= BB_CACHE_KEY_MAX);

    bb_cache_config_t cfg = {
        .key       = long_key,
        .snapshot  = NULL,
        .snap_size = sizeof(synth_snap_t),
        .flags     = BB_CACHE_FLAG_NONE,
    };
    bb_err_t err = bb_cache_register(&cfg);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL_UINT(0, bb_cache_count());
}

// A key at exactly BB_CACHE_KEY_MAX-1 chars (the longest that fits with the
// NUL) must succeed — the rejection boundary is strlen(key) >= KEY_MAX, not
// KEY_MAX itself.
void test_bb_cache_register_key_at_max_length_boundary_succeeds(void)
{
    reset_all();

    char max_key[BB_CACHE_KEY_MAX];
    memset(max_key, 'k', sizeof(max_key) - 1);
    max_key[sizeof(max_key) - 1] = '\0';
    TEST_ASSERT_EQUAL_UINT((size_t)(BB_CACHE_KEY_MAX - 1), strlen(max_key));

    bb_cache_config_t cfg = {
        .key       = max_key,
        .snapshot  = NULL,
        .snap_size = sizeof(synth_snap_t),
        .flags     = BB_CACHE_FLAG_NONE,
    };
    bb_err_t err = bb_cache_register(&cfg);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_UINT(1, bb_cache_count());
}

// ---------------------------------------------------------------------------
// cfg->out_first_time (B1-1118: bb_cache_register/_ex collapsed into a
// single bb_cache_register(cfg), with first-time reporting now a nullable
// cfg->out_first_time out-param instead of a separate _ex entry point).
// ---------------------------------------------------------------------------

void test_bb_cache_register_out_first_time_true_on_first_registration(void)
{
    reset_all();
    bool first_time = false;
    bb_cache_config_t cfg = {
        .key            = "test.first_time",
        .snapshot       = NULL,
        .snap_size      = sizeof(synth_snap_t),
        .flags          = BB_CACHE_FLAG_NONE,
        .out_first_time = &first_time,
    };
    bb_err_t err = bb_cache_register(&cfg);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_TRUE(first_time);
}

void test_bb_cache_register_out_first_time_false_on_reregister(void)
{
    reset_all();
    bool first_time = false;
    bb_cache_config_t cfg = {
        .key            = "test.reregister",
        .snapshot       = NULL,
        .snap_size      = sizeof(synth_snap_t),
        .flags          = BB_CACHE_FLAG_NONE,
        .out_first_time = &first_time,
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));
    TEST_ASSERT_TRUE(first_time);

    // Idempotent re-register of the same (still-live) key: still BB_OK, but
    // this call did NOT perform the first-time registration.
    first_time = true;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));
    TEST_ASSERT_FALSE(first_time);
    TEST_ASSERT_EQUAL_UINT(1, bb_cache_count());
}

// out_first_time is nullable -- callers that don't need first-time detection
// (the vast majority) may leave it unset (NULL, via zero-init) with no
// behavior change from the pre-collapse bb_cache_register().
void test_bb_cache_register_out_first_time_null_is_optional(void)
{
    reset_all();
    bb_cache_config_t cfg = {
        .key       = "test.no_out_param",
        .snapshot  = NULL,
        .snap_size = sizeof(synth_snap_t),
        .flags     = BB_CACHE_FLAG_NONE,
        // .out_first_time left NULL (zero-init).
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));
    TEST_ASSERT_EQUAL_UINT(1, bb_cache_count());
}

// ---------------------------------------------------------------------------
// PR-4a-0: owned+fallback cold-start seed (bb_cache.h tri-state, third combo:
// snapshot != NULL AND snap_size > 0). Covers: seed-on-first-read, seed
// invoked at most once, a real write always wins and never re-triggers the
// seed, the seed does NOT count toward bb_cache_update's out_changed/has_value
// guard, and a fallback getter returning NULL leaves the owned buffer
// untouched (zero-initialized) without retrying. Note: bb_cache core has no
// observer/notify machinery for this seed -- "the seed does not fire
// observers" is trivially true; there is nothing to subscribe to.
// ---------------------------------------------------------------------------

static synth_snap_t s_cs_fallback_val = {.value = 77, .flag = true, .ratio = 4.25f};
static int  s_cs_snapshot_calls;
static bool s_cs_snapshot_return_null;

static const void *cs_fallback_snapshot(void)
{
    s_cs_snapshot_calls++;
    if (s_cs_snapshot_return_null) return NULL;
    return &s_cs_fallback_val;
}

static void cs_reset(void)
{
    s_cs_snapshot_calls = 0;
    s_cs_snapshot_return_null = false;
}

// First read while unpopulated seeds the owned buffer from the fallback
// getter -- the endpoint is never empty at cold start.
void test_bb_cache_owned_fallback_seeds_on_first_read(void)
{
    reset_all();
    cs_reset();

    TEST_ASSERT_EQUAL_INT(BB_OK,
        reg("test.cs.seed", cs_fallback_snapshot, sizeof(synth_snap_t)));

    synth_snap_t out = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.cs.seed", &out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(77, out.value);
    TEST_ASSERT_EQUAL_INT(1, s_cs_snapshot_calls);
}

// The seed fires at most ONCE while unpopulated -- repeated reads before any
// real write must not re-invoke the fallback getter, whether or not the read
// path memoizes (bb_cache_get_raw has no memoization at all -- it calls
// maybe_seed_fallback() on every call -- so this specifically proves
// maybe_seed_fallback()'s OWN idempotency guard, not a caller-side cache).
void test_bb_cache_owned_fallback_seed_invoked_once_across_reads(void)
{
    reset_all();
    cs_reset();

    reg("test.cs.once", cs_fallback_snapshot, sizeof(synth_snap_t));

    synth_snap_t out = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.cs.once", &out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.cs.once", &out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.cs.once", &out, sizeof(out)));

    TEST_ASSERT_EQUAL_INT(1, s_cs_snapshot_calls);
}

// A real bb_cache_update always wins and permanently stops the seed path
// (strict boot bridge -- no expiry, never re-runs after a real write).
void test_bb_cache_owned_fallback_real_write_wins_and_stops_seeding(void)
{
    reset_all();
    cs_reset();

    reg("test.cs.write", cs_fallback_snapshot, sizeof(synth_snap_t));

    // Trigger the seed via a read before any real write.
    synth_snap_t out = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.cs.write", &out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(1, s_cs_snapshot_calls);

    synth_snap_t written = {.value = 123, .flag = false, .ratio = 9.5f};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.cs.write", .snap = &written }));

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.cs.write", &out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(123, out.value);

    // The real write (and the read that followed it) must never re-invoke
    // the fallback getter.
    TEST_ASSERT_EQUAL_INT(1, s_cs_snapshot_calls);
}

// The seed does NOT set has_value -- bb_cache_update's out_changed change-
// detection guard is unaffected by it, so the entry's first REAL write still reports
// changed=true unconditionally, even when the written bytes are byte-
// identical to the seeded value.
void test_bb_cache_owned_fallback_first_real_write_reports_changed_even_matching_seed(void)
{
    reset_all();
    cs_reset();

    reg("test.cs.changed", cs_fallback_snapshot, sizeof(synth_snap_t));

    synth_snap_t out = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.cs.changed", &out, sizeof(out)));  // seeds

    bool changed = false;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_update(&(bb_cache_update_t){ .key = "test.cs.changed", .snap = &s_cs_fallback_val, .out_changed = &changed }));
    TEST_ASSERT_TRUE_MESSAGE(changed,
        "first REAL write must report changed=true even if it matches the cold-start seed");
}

// A fallback getter returning NULL leaves the owned buffer untouched (still
// zero-initialized from register) and must not be retried on a later read.
void test_bb_cache_owned_fallback_snapshot_returns_null_leaves_zero_value(void)
{
    reset_all();
    cs_reset();
    s_cs_snapshot_return_null = true;

    reg("test.cs.nullsnap", cs_fallback_snapshot, sizeof(synth_snap_t));

    synth_snap_t out = {.value = -1};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.cs.nullsnap", &out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(0, out.value);  // owned buffer stayed zero-initialized
    TEST_ASSERT_EQUAL_INT(1, s_cs_snapshot_calls);

    // Second read must not retry the failed seed.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.cs.nullsnap", &out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(1, s_cs_snapshot_calls);
}

// Plain OWNED mode still requires a non-zero snap_size (snapshot == NULL,
// snap_size == 0 is invalid) -- unaffected by the tri-state register()
// refactor.
void test_bb_cache_register_owned_mode_requires_snap_size(void)
{
    reset_all();
    bb_cache_config_t cfg = {
        .key       = "test.cs.needsize",
        .snapshot  = NULL,
        .snap_size = 0,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_register(&cfg));
    TEST_ASSERT_FALSE(bb_cache_exists("test.cs.needsize"));
}

// ---------------------------------------------------------------------------
// B1-... PR-4a: bb_cache_update's out_changed change-detection + bb_cache_exists
// ---------------------------------------------------------------------------

// First write since register must report changed=true, even against a
// zero-initialized owned buffer (has_value guard, no false negative).
void test_bb_cache_update_out_changed_first_write_all_zero_reports_changed(void)
{
    reset_all();
    reg("test.ex.firstzero", NULL, sizeof(synth_snap_t));

    synth_snap_t zero = {0};
    bool changed = false;
    bb_err_t err = bb_cache_update(&(bb_cache_update_t){ .key = "test.ex.firstzero", .snap = &zero, .out_changed = &changed });
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_TRUE(changed);
}

// Identical rewrite (same bytes) must report changed=false.
void test_bb_cache_update_out_changed_identical_rewrite_reports_unchanged(void)
{
    reset_all();
    reg("test.ex.identical", NULL, sizeof(synth_snap_t));

    synth_snap_t s = {.value = 1, .flag = true, .ratio = 0.5f};
    bool changed = true;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.ex.identical", .snap = &s, .out_changed = &changed }));
    TEST_ASSERT_TRUE(changed);  // first write

    changed = true;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.ex.identical", .snap = &s, .out_changed = &changed }));
    TEST_ASSERT_FALSE(changed);  // same bytes rewritten
}

// A different value on a subsequent write must report changed=true.
void test_bb_cache_update_out_changed_different_value_reports_changed(void)
{
    reset_all();
    reg("test.ex.diff", NULL, sizeof(synth_snap_t));

    synth_snap_t s1 = {.value = 1, .flag = false, .ratio = 0.0f};
    bool changed = false;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.ex.diff", .snap = &s1, .out_changed = &changed }));
    TEST_ASSERT_TRUE(changed);

    synth_snap_t s2 = {.value = 2, .flag = false, .ratio = 0.0f};
    changed = false;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.ex.diff", .snap = &s2, .out_changed = &changed }));
    TEST_ASSERT_TRUE(changed);
}

// out_changed == NULL must not crash and must still copy in the value.
void test_bb_cache_update_null_out_changed_does_not_crash(void)
{
    reset_all();
    reg("test.ex.nullout", NULL, sizeof(synth_snap_t));

    synth_snap_t s = {.value = 3, .flag = true, .ratio = 1.0f};
    bb_err_t err = bb_cache_update(&(bb_cache_update_t){ .key = "test.ex.nullout", .snap = &s });
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    synth_snap_t out = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.ex.nullout", &out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(3, out.value);
}

static synth_snap_t s_env_getter_val = {.value = 3, .flag = true, .ratio = 2.0f};

static const void *env_getter_snapshot(void)
{
    return &s_env_getter_val;
}

// Getter-mode keys report changed=false always (no owned bytes to diff).
void test_bb_cache_update_getter_mode_out_changed_reports_unchanged(void)
{
    reset_all();
    reg("test.ex.getter", env_getter_snapshot, 0);

    synth_snap_t s = {.value = 4, .flag = false, .ratio = 0.0f};
    bool changed = true;
    bb_err_t err = bb_cache_update(&(bb_cache_update_t){ .key = "test.ex.getter", .snap = &s, .out_changed = &changed });
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_FALSE(changed);
}

// Getter-mode with out_changed == NULL must not crash (the getter-mode branch
// also guards the out_changed pointer independently of owned mode).
void test_bb_cache_update_getter_mode_null_out_changed_does_not_crash(void)
{
    reset_all();
    reg("test.ex.getter.nullout", env_getter_snapshot, 0);

    synth_snap_t s = {.value = 4, .flag = false, .ratio = 0.0f};
    bb_err_t err = bb_cache_update(&(bb_cache_update_t){ .key = "test.ex.getter.nullout", .snap = &s });
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

// NULL key and NULL snap both independently return BB_ERR_INVALID_ARG.
void test_bb_cache_update_null_args_returns_invalid_arg(void)
{
    reset_all();
    synth_snap_t s = {0};
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_update(&(bb_cache_update_t){ .key = NULL, .snap = &s }));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_update(&(bb_cache_update_t){ .key = "test.ex.nullargs", .snap = NULL }));
}

// A NULL req pointer itself (as opposed to a NULL field inside a non-NULL
// req) must also return BB_ERR_INVALID_ARG.
void test_bb_cache_update_null_req_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_update(NULL));
}

// A nonzero .ts_ms overrides the "now" stamp with the caller-supplied sample
// time (e.g. an ingress source's own timestamp). ts_ms has no direct public
// accessor (B1-1119 removed the JSON envelope that used to expose it), so
// the override is observed indirectly via bb_cache_is_stale(): an AGE_OUT key
// stamped with an ancient ts_ms must immediately report stale against a
// millisecond-scale stale window -- if the override were silently ignored
// (falling back to "now"), the key would still report fresh.
void test_bb_cache_update_ts_ms_nonzero_overrides_stamp(void)
{
    reset_all();
    bb_cache_config_t cfg = {
        .key       = "test.tsms.override",
        .snapshot  = NULL,
        .snap_size = sizeof(synth_snap_t),
        .flags     = BB_CACHE_FLAG_NONE,
        .eviction  = {
            .policy       = BB_CACHE_EVICT_AGE_OUT,
            .stale_age_ms = 1,
            .evict_age_ms = 1000000000,  // effectively never evicts in this test
        },
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));

    synth_snap_t s = {.value = 1, .flag = false, .ratio = 0.0f};
    const int64_t ancient_ts = 1;  // epoch ms == 1: decades in the past
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_update(&(bb_cache_update_t){
            .key = "test.tsms.override", .snap = &s, .ts_ms = ancient_ts }));

    bool stale = false;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_is_stale("test.tsms.override", &stale));
    TEST_ASSERT_TRUE_MESSAGE(stale, "ts_ms override must have taken effect, not fallen back to \"now\"");
}

// bb_cache_exists rejects a NULL key.
void test_bb_cache_exists_null_key_returns_false(void)
{
    reset_all();
    TEST_ASSERT_FALSE(bb_cache_exists(NULL));
}

// bb_cache_update's basic (no out_changed) behavior: succeeds, copies in
// the value, unknown key -> BB_ERR_NOT_FOUND.
void test_bb_cache_update_basic_behavior(void)
{
    reset_all();
    reg("test.ex.wrapper", NULL, sizeof(synth_snap_t));

    synth_snap_t s = {.value = 6, .flag = true, .ratio = 2.0f};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.ex.wrapper", .snap = &s }));

    synth_snap_t out = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.ex.wrapper", &out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(6, out.value);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_cache_update(&(bb_cache_update_t){ .key = "no.such.ex.topic", .snap = &s }));
}

// bb_cache_exists: true for a registered key, false for an unregistered key.
void test_bb_cache_exists_registered_and_unregistered(void)
{
    reset_all();
    TEST_ASSERT_FALSE(bb_cache_exists("test.ex.exists"));

    reg("test.ex.exists", NULL, sizeof(synth_snap_t));
    TEST_ASSERT_TRUE(bb_cache_exists("test.ex.exists"));

    TEST_ASSERT_FALSE(bb_cache_exists("no.such.ex.exists.topic"));
}

// ---------------------------------------------------------------------------
// B1-592 PR-A1: bb_cache_delete (runtime eviction primitive)
// ---------------------------------------------------------------------------

// Deleting an existing key removes it from the registry: exists() flips to
// false and get_raw returns BB_ERR_NOT_FOUND.
void test_bb_cache_delete_existing_removes_entry(void)
{
    reset_all();
    reg("test.del.basic", NULL, sizeof(synth_snap_t));
    synth_snap_t s = {.value = 1, .flag = true, .ratio = 0.5f};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.del.basic", .snap = &s }));
    TEST_ASSERT_TRUE(bb_cache_exists("test.del.basic"));

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_delete("test.del.basic"));

    TEST_ASSERT_FALSE(bb_cache_exists("test.del.basic"));
    synth_snap_t out = {0};
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_cache_get_raw("test.del.basic", &out, sizeof(out)));
}

// Deleting a key that was never registered returns BB_ERR_NOT_FOUND.
void test_bb_cache_delete_missing_returns_not_found(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_cache_delete("no.such.key"));
}

// NULL key returns BB_ERR_INVALID_ARG.
void test_bb_cache_delete_null_key_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_delete(NULL));
}

// Deleting a key and re-registering it under the SAME name must produce a
// fresh, never-used slot: has_value=false (no stale change-detect memory).
// Compared against a topic that was registered fresh and never touched.
void test_bb_cache_delete_then_reregister_same_key_is_fresh(void)
{
    reset_all();

    // Register and write the FIRST incarnation.
    reg("test.del.reuse", NULL, sizeof(synth_snap_t));
    synth_snap_t s1 = {.value = 111, .flag = true, .ratio = 9.5f};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.del.reuse", .snap = &s1 }));

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_delete("test.del.reuse"));

    // Re-register the SAME key -- second incarnation.
    TEST_ASSERT_EQUAL_INT(BB_OK, reg("test.del.reuse", NULL, sizeof(synth_snap_t)));

    // A control topic, registered fresh and never touched, for comparison.
    TEST_ASSERT_EQUAL_INT(BB_OK, reg("test.del.control", NULL, sizeof(synth_snap_t)));

    // Both the reused and control keys must report the same out_changed
    // first-write semantics (has_value reset to false by the delete).
    synth_snap_t zero = {0};
    bool reused_changed = false, control_changed = false;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){
        .key = "test.del.reuse", .snap = &zero, .out_changed = &reused_changed }));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){
        .key = "test.del.control", .snap = &zero, .out_changed = &control_changed }));
    TEST_ASSERT_TRUE(reused_changed);
    TEST_ASSERT_TRUE(control_changed);
    TEST_ASSERT_EQUAL(control_changed, reused_changed);

    // Stored bytes must match the control topic -- no stale bytes bleeding
    // through from the deleted first incarnation.
    synth_snap_t reused_out = {0}, control_out = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.del.reuse", &reused_out, sizeof(reused_out)));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.del.control", &control_out, sizeof(control_out)));
    TEST_ASSERT_EQUAL_INT(control_out.value, reused_out.value);
    TEST_ASSERT_EQUAL(control_out.flag, reused_out.flag);
    TEST_ASSERT_EQUAL_FLOAT(control_out.ratio, reused_out.ratio);
}

// bb_cache_foreach snapshots keys BY VALUE -- a delete that races the
// lock-released window (between the snapshot and cb invocation) must not
// corrupt an in-flight cb's key pointer. This test doesn't need real
// concurrency to prove the copy-by-value contract: it deletes a key from
// INSIDE the foreach callback itself (the callback runs with s_reg_lock
// released, per the header contract, so this is legal) and asserts the
// callback's own key argument is still intact afterward -- if foreach handed
// out a raw pointer into s_entries[].key, the delete would have zeroed the
// first byte of the very string cb is holding.
static char s_foreach_del_seen_key[BB_CACHE_KEY_MAX];
static bool s_foreach_del_deleted_ok;

static void foreach_delete_cb(const char *key, void *ctx)
{
    (void)ctx;
    if (strcmp(key, "test.del.foreach.victim") == 0) {
        // Delete the very key cb was just handed -- if foreach snapshotted a
        // raw pointer, this would mutate key's backing bytes mid-callback.
        s_foreach_del_deleted_ok = (bb_cache_delete("test.del.foreach.victim") == BB_OK);
        strncpy(s_foreach_del_seen_key, key, sizeof(s_foreach_del_seen_key) - 1);
        s_foreach_del_seen_key[sizeof(s_foreach_del_seen_key) - 1] = '\0';
    }
}

void test_bb_cache_foreach_copy_by_value_survives_delete_during_callback(void)
{
    reset_all();
    s_foreach_del_seen_key[0] = '\0';
    s_foreach_del_deleted_ok = false;

    reg("test.del.foreach.victim", NULL, sizeof(synth_snap_t));

    bb_err_t err = bb_cache_foreach(foreach_delete_cb, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_TRUE(s_foreach_del_deleted_ok);
    // The key string cb held must be unaffected by the delete that happened
    // while cb was executing -- proves the copy was on foreach's own stack,
    // not a pointer into the now-freed/reused registry slot.
    TEST_ASSERT_EQUAL_STRING("test.del.foreach.victim", s_foreach_del_seen_key);
    TEST_ASSERT_FALSE(bb_cache_exists("test.del.foreach.victim"));
}

// Table-full still returns BB_ERR_NO_SPACE after a delete-and-refill cycle --
// confirms bb_cache_register's existing full-table guard (already present,
// verified) is untouched by the delete primitive: filling the table, then
// deleting one slot and refilling it, then attempting one more registration
// must still fail with NO_SPACE.
void test_bb_cache_delete_table_full_guard_still_returns_no_space(void)
{
    reset_all();
    char key_buf[32];

    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        snprintf(key_buf, sizeof(key_buf), "test.del.fill.%d", i);
        TEST_ASSERT_EQUAL_INT(BB_OK, reg(key_buf, NULL, sizeof(synth_snap_t)));
    }

    // Delete one slot, then refill it -- table is full again.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_delete("test.del.fill.0"));
    TEST_ASSERT_EQUAL_INT(BB_OK, reg("test.del.fill.0", NULL, sizeof(synth_snap_t)));

    bb_err_t err = reg("test.del.fill.overflow", NULL, sizeof(synth_snap_t));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
}

// Deleting a getter-mode entry (owned == NULL) must be a clean no-crash path
// through free_entry_locked's bb_mem_free(e->owned) call -- bb_mem_free(NULL)
// is required to be a safe no-op, and getter-mode entries never allocate an
// owned buffer.
void test_bb_cache_delete_getter_mode_entry_no_crash(void)
{
    reset_all();
    reg("test.del.getter", get_raw_test_getter, 0);
    TEST_ASSERT_TRUE(bb_cache_exists("test.del.getter"));

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_delete("test.del.getter"));
    TEST_ASSERT_FALSE(bb_cache_exists("test.del.getter"));
}

// ---------------------------------------------------------------------------
// B1-592 firmware-review coverage gate: bb_cache platform impl newly gated at
// 100% branch (Makefile filter now includes platform/{espidf,host}/bb_cache).
// These deterministic (non-MT) tests close simple arg-validation and OOM-
// injection branches that were always compiled but previously invisible to
// the coverage report.
// ---------------------------------------------------------------------------

static void *fail_alloc(size_t sz) { (void)sz; return NULL; }

// Owned-mode registration's bb_calloc_prefer_spiram failure path.
void test_bb_cache_register_owned_alloc_failure_returns_no_space(void)
{
    reset_all();
    bb_mem_set_alloc_hook(fail_alloc);
    bb_cache_config_t cfg = {
        .key       = "test.cs.allocfail",
        .snapshot  = NULL,
        .snap_size = sizeof(synth_snap_t),
        .flags     = BB_CACHE_FLAG_NONE,
    };
    bb_err_t err = bb_cache_register(&cfg);
    bb_mem_set_alloc_hook(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_FALSE(bb_cache_exists("test.cs.allocfail"));
}

// ---------------------------------------------------------------------------
// B1-592 firmware-review fix: reader-vs-delete TOCTOU/UAF closed by the
// tombstone + generation-guard invariant (see find_entry_locked_ref()'s doc
// comment in bb_cache_espidf.c). Every reader (bb_cache_update/get_raw)
// captures an (entry, generation) pair under s_reg_lock, then re-validates
// BOTH key and generation immediately after acquiring the entry's own lock
// -- so a concurrent bb_cache_delete() (which never destroys the mutex, only
// bumps generation and frees the slot) can never be observed as a
// destroyed-mutex UB or as another key's bytes under this key's name.
//
// This test hammers bb_cache_get_raw + bb_cache_update on a single churning
// slot from one thread while a second thread repeatedly deletes and
// re-registers it -- alternating between the SAME key (same-incarnation
// reuse) and a DIFFERENT key (cross-key slot reuse, the more dangerous case:
// a reader must never observe the other key's bytes under its own key name).
//
// NOT A HARD REGRESSION GUARD without a race detector -- same caveat as the
// B1-568 test above: schedule-dependent, no ASan/TSan on this host build.
// Its value is in exercising the exact interleaving the fix targets (many
// iterations, no artificial synchronization slowing the race window) rather
// than guaranteeing detection on every run.
// ---------------------------------------------------------------------------

#define B1_592_MT_ITERS 6000

typedef struct { int value; } mt_churn_snap_t;

typedef struct {
    int unexpected_err_count; // any error other than BB_OK/BB_ERR_NOT_FOUND
    int poisoned_count;       // observed a value that belongs to the OTHER key
} mt_churn_result_t;

// Registers key so every surviving reader entry point can be exercised
// against the churning slot.
static bb_err_t mt_churn_register(const char *key)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = NULL,
        .snap_size = sizeof(mt_churn_snap_t),
        .flags     = BB_CACHE_FLAG_NONE,
    };
    return bb_cache_register(&cfg);
}

static void *b1_592_deleter_fn(void *arg)
{
    (void)arg;
    // 3-slot cycle: churn, churn, churnB -- unconditionally deletes BOTH
    // possible occupants every iteration before re-registering, guaranteeing
    // a single-occupant invariant so the slot is genuinely reused every
    // iteration. The repeated "churn" entries exercise SAME-key
    // delete+reregister (same incarnation-vs-incarnation identity check);
    // the churn->churnB and churnB->churn transitions exercise cross-key
    // slot reuse (the more dangerous case).
    static const char *pattern[3] = {
        "test.mt.churn", "test.mt.churn", "test.mt.churnB"
    };
    for (int i = 0; i < B1_592_MT_ITERS; i++) {
        const char *cur = pattern[i % 3];
        int expected = (cur[strlen(cur) - 1] == 'B') ? 222 : 111;

        bb_cache_delete("test.mt.churn");
        bb_cache_delete("test.mt.churnB");
        mt_churn_register(cur);
        mt_churn_snap_t s = { .value = expected };
        bb_cache_update(&(bb_cache_update_t){ .key = cur, .snap = &s });
    }

    // Leave a final, stable incarnation of "test.mt.churn" so a post-join
    // sanity check has something deterministic to see.
    bb_cache_delete("test.mt.churn");
    bb_cache_delete("test.mt.churnB");
    mt_churn_register("test.mt.churn");
    mt_churn_snap_t s = { .value = 111 };
    bb_cache_update(&(bb_cache_update_t){ .key = "test.mt.churn", .snap = &s });
    return NULL;
}

// Exercises every surviving reader entry point that captures (entry,
// generation) via find_entry_locked_ref() and re-validates under e->lock:
// get_raw, update. Any return other than BB_OK/BB_ERR_NOT_FOUND is
// unexpected; observing the OTHER key's value under this key's name is
// poisoning.
static void mt_churn_check_one(const char *key, int expected, mt_churn_result_t *res)
{
    mt_churn_snap_t raw = {0};
    bb_err_t rerr = bb_cache_get_raw(key, &raw, sizeof(raw));
    if (rerr == BB_OK) {
        if (raw.value != 0 && raw.value != expected) res->poisoned_count++;
    } else if (rerr != BB_ERR_NOT_FOUND) {
        res->unexpected_err_count++;
    }

    bool changed = false;
    mt_churn_snap_t upd = { .value = expected };
    bb_err_t uerr = bb_cache_update(&(bb_cache_update_t){
        .key = key, .snap = &upd, .out_changed = &changed });
    if (uerr != BB_OK && uerr != BB_ERR_NOT_FOUND) res->unexpected_err_count++;
}

static void *b1_592_reader_fn(void *arg)
{
    mt_churn_result_t *res = (mt_churn_result_t *)arg;
    for (int i = 0; i < B1_592_MT_ITERS; i++) {
        mt_churn_check_one("test.mt.churn", 111, res);
        mt_churn_check_one("test.mt.churnB", 222, res);
    }
    return NULL;
}

void test_bb_cache_delete_reader_race_never_poisons_or_crashes(void)
{
    reset_all();

    mt_churn_result_t reader_res_a = {0};
    mt_churn_result_t reader_res_b = {0};

    // Two reader threads (not just one) to widen the race window further.
    pthread_t deleter_tid, reader_tid_a, reader_tid_b;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&deleter_tid, NULL, b1_592_deleter_fn, NULL));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&reader_tid_a, NULL, b1_592_reader_fn, &reader_res_a));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&reader_tid_b, NULL, b1_592_reader_fn, &reader_res_b));

    pthread_join(deleter_tid, NULL);
    pthread_join(reader_tid_a, NULL);
    pthread_join(reader_tid_b, NULL);

    TEST_ASSERT_EQUAL_INT(0, reader_res_a.unexpected_err_count);
    TEST_ASSERT_EQUAL_INT(0, reader_res_a.poisoned_count);
    TEST_ASSERT_EQUAL_INT(0, reader_res_b.unexpected_err_count);
    TEST_ASSERT_EQUAL_INT(0, reader_res_b.poisoned_count);

    // Sanity: the deleter's final state left "test.mt.churn" registered with
    // its own value, "test.mt.churnB" deleted.
    TEST_ASSERT_TRUE(bb_cache_exists("test.mt.churn"));
    TEST_ASSERT_FALSE(bb_cache_exists("test.mt.churnB"));
}

// ---------------------------------------------------------------------------
// Deterministic tombstone/generation-mismatch coverage (line-coverage
// follow-up to the B1-592 firmware-review fix above). The multi-threaded race
// test above is schedule-dependent -- it reliably hits these mismatch
// branches on some hosts but not others within a bounded iteration count
// (observed: hit on macOS, missed on the Linux CI runner, which is what
// tripped Coveralls' patch-line gate on the call sites below despite 100%
// branch coverage). bb_cache_test_set_race_hook() (BB_CACHE_TESTING) lets
// these tests deterministically, single-threadedly reproduce the exact
// interleaving the guard exists to close: delete-and-re-register the SAME
// key between find_entry_locked_ref()'s capture and the entry's own lock
// acquisition, guaranteeing a generation mismatch on the very next check.
// ---------------------------------------------------------------------------
void bb_cache_test_set_race_hook(void (*hook)(const char *key));

typedef struct { int value; } race_snap_t;

static bb_err_t race_register(const char *key)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = NULL,
        .snap_size = sizeof(race_snap_t),
        .flags     = BB_CACHE_FLAG_NONE,
    };
    return bb_cache_register(&cfg);
}

// Fires once (one-shot, see fire_test_race_hook()) inside the target
// function, between its find_entry_locked_ref() capture and its
// pthread_mutex_lock(&e->lock) -- deletes and re-registers the same key so
// the generation the caller captured is now stale.
static void race_delete_and_reregister(const char *key)
{
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_delete(key));
    TEST_ASSERT_EQUAL_INT(BB_OK, race_register(key));
}

// Fires once, deletes the key WITHOUT re-registering it -- distinct from
// race_delete_and_reregister above. entry_matches_locked's first (short-
// circuiting) condition, e->key[0] != '\0', is what actually catches this
// case: bb_cache_delete() clears key[0] as its last step, so a reader that
// takes e->lock after a bare delete (no re-register) sees a tombstoned slot
// and bails on the key-empty check before ever comparing generation or key
// string -- a different branch outcome than the generation-mismatch one the
// delete+reregister hook exercises.
static void race_delete_only(const char *key)
{
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_delete(key));
}

// Covers bb_cache_update's own inline tombstone-mismatch branch.
void test_bb_cache_update_delete_race_returns_not_found(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_OK, race_register("test.race.update"));

    bb_cache_test_set_race_hook(race_delete_and_reregister);

    race_snap_t s = { .value = 42 };
    bb_err_t err = bb_cache_update(&(bb_cache_update_t){
        .key = "test.race.update", .snap = &s });

    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, err);
    TEST_ASSERT_TRUE(bb_cache_exists("test.race.update"));
}

// Covers bb_cache_get_raw's own inline tombstone-mismatch (generation)
// branch.
void test_bb_cache_get_raw_delete_race_returns_not_found(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_OK, race_register("test.race.getraw"));

    bb_cache_test_set_race_hook(race_delete_and_reregister);

    race_snap_t out = {0};
    bb_err_t err = bb_cache_get_raw("test.race.getraw", &out, sizeof(out));

    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, err);
    TEST_ASSERT_TRUE(bb_cache_exists("test.race.getraw"));
}

// Covers entry_matches_locked's key-empty (tombstoned, not re-registered)
// short-circuit branch via bb_cache_get_raw -- distinct from every
// generation-mismatch race test above. A bare delete with no re-register
// leaves key[0] == '\0', so this exercises the FIRST condition's false path
// rather than the second.
void test_bb_cache_get_raw_delete_only_race_returns_not_found(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_OK, race_register("test.race.delonly"));

    bb_cache_test_set_race_hook(race_delete_only);

    race_snap_t out = {0};
    bb_err_t err = bb_cache_get_raw("test.race.delonly", &out, sizeof(out));

    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, err);
    // Hook did NOT re-register -- the slot stays tombstoned.
    TEST_ASSERT_FALSE(bb_cache_exists("test.race.delonly"));
}

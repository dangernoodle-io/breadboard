// Host equality harness for bb_cache.
//
// Proves the same-serializer guarantee: event-side serialization (via
// bb_cache_serialize_into on a fresh obj) byte-equals REST-side serialization
// (bb_cache_serialize_into on another fresh obj) for every registered topic.
//
// Synthetic topic registered here; real topics (net.health, diag.boot,
// update.available, health.display) wire in below in the TOPIC TABLE
// comment with one entry each.
//
// EXTENSION POINT: To add a real topic, add a row to the `s_topics[]` table
// below following the bb_cache_fidelity_topic_t shape.

#include "unity.h"
#include "bb_cache.h"
#include "bb_clock.h"
#include "bb_diag_event_priv.h"
#include "bb_display_info_event_priv.h"
#include "bb_event.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_net_health.h"
#include "bb_ota_check_internal.h"
#include "bb_mem_test.h"
#include "bb_json_test_hooks.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Test reset hook — declared in bb_cache_espidf.c (BB_CACHE_TESTING)
// ---------------------------------------------------------------------------
void bb_cache_reset_for_test(void);

// Test helper: mirrors the pre-config-struct bb_cache_register() shape for
// tests that don't care about flags (defaults to BB_CACHE_FLAG_SSE, matching
// the old legacy-form behaviour these tests were written against).
static bb_err_t reg(const char *key, const void *(*snapshot)(void),
                     size_t snap_size, bb_cache_serialize_fn serialize)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = snapshot,
        .snap_size = snap_size,
        .serialize = serialize,
        .flags     = BB_CACHE_FLAG_SSE,
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

static void synth_serialize(bb_json_t obj, const void *snap)
{
    const synth_snap_t *s = (const synth_snap_t *)snap;
    bb_json_obj_set_int   (obj, "value", s->value);
    bb_json_obj_set_bool  (obj, "flag",  s->flag);
    bb_json_obj_set_number(obj, "ratio", s->ratio);
}

// ---------------------------------------------------------------------------
// Generic fidelity topic descriptor
// ---------------------------------------------------------------------------

typedef struct {
    const char           *name;
    const void          *(*snapshot)(void);   // NULL = owned mode
    size_t                snap_size;
    bb_cache_serialize_fn serialize;
    const void           *initial_snap;       // data to seed via bb_cache_update (owned mode)
} bb_cache_fidelity_topic_t;

// ---------------------------------------------------------------------------
// TOPIC TABLE
// Synthetic entry here; migrate real topics below this line in later steps.
// ---------------------------------------------------------------------------

static const synth_snap_t s_synth_initial = {
    .value = 42,
    .flag  = true,
    .ratio = 0.75,
};

static const bb_net_health_status_t s_net_health_initial = {
    .state                  = BB_WIFI_LINK_GOOD,
    .early_warning          = false,
    .throttled              = false,
    .rssi                   = -55,
    .mqtt_connected         = true,
    .mqtt_reconnect_count   = 0,
    .last_disconnect_reason = 0,
    .disc_age_s             = 0,
    .mqtt_disc_age_s        = 0,
    .mqtt_disc_reason       = 0,
    .mqtt_tls_fail          = 0,
    .lost_ip_recoveries     = 0,
    .lost_ip_age_s          = 0,
    .egress_dead_recoveries = 0,
};

static const bb_diag_boot_snap_t s_diag_boot_initial = {
    .reset_reason    = "power-on",
    .wdt_resets      = 0,
    .panic_available = false,
    .panic_boots_since = 0,
    .pending_verify  = false,
    .rolled_back     = false,
};

static const bb_display_snap_t s_display_initial = {
    .present = true,
    .panel   = "mock",
    .width   = 320,
    .height  = 240,
    .enabled = true,
};

static const bb_ota_check_snap_t s_update_initial = {
    .current       = "",
    .latest        = "",
    .download_url  = "",
    .available     = false,
    .ts            = 0,
    .last_check_ok = false,
    .enabled       = false,
    .outcome       = "unknown",
    .last_check_ts = 0,
};

static const bb_cache_fidelity_topic_t s_topics[] = {
    {
        .name         = "test.synth",
        .snapshot     = NULL,                   // owned mode
        .snap_size    = sizeof(synth_snap_t),
        .serialize    = synth_serialize,
        .initial_snap = &s_synth_initial,
    },
    {
        .name         = "net.health",
        .snapshot     = NULL,
        .snap_size    = sizeof(bb_net_health_status_t),
        .serialize    = bb_net_health_emit,
        .initial_snap = &s_net_health_initial,
    },
    {
        .name         = BB_DIAG_BOOT_TOPIC,
        .snapshot     = NULL,
        .snap_size    = sizeof(bb_diag_boot_snap_t),
        .serialize    = bb_diag_boot_serialize,
        .initial_snap = &s_diag_boot_initial,
    },
    {
        .name         = BB_OTA_CHECK_TOPIC,
        .snapshot     = NULL,
        .snap_size    = sizeof(bb_ota_check_snap_t),
        .serialize    = bb_ota_check_serialize,
        .initial_snap = &s_update_initial,
    },
    {
        .name         = BB_DISPLAY_INFO_TOPIC,
        .snapshot     = NULL,
        .snap_size    = sizeof(bb_display_snap_t),
        .serialize    = bb_display_serialize,
        .initial_snap = &s_display_initial,
    },
};

#define N_TOPICS ((int)(sizeof(s_topics) / sizeof(s_topics[0])))

// ---------------------------------------------------------------------------
// setUp / tearDown
// ---------------------------------------------------------------------------

static void reset_all(void)
{
    bb_cache_reset_for_test();
    bb_event_init(NULL);
}

// ---------------------------------------------------------------------------
// Generic equality assertion
// ---------------------------------------------------------------------------

// Registers a topic, seeds its initial snapshot, then asserts that two
// independent calls to bb_cache_serialize_into produce byte-identical JSON.
static void assert_fidelity(const bb_cache_fidelity_topic_t *t)
{
    bb_err_t err;

    bb_cache_config_t cfg = {
        .key       = t->name,
        .snapshot  = t->snapshot,
        .snap_size = t->snap_size,
        .serialize = t->serialize,
        .flags     = BB_CACHE_FLAG_SSE,
    };
    err = bb_cache_register(&cfg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, err, t->name);

    // Seed owned-mode entries; getter-mode entries provide their own data.
    if (!t->snapshot && t->initial_snap) {
        err = bb_cache_update(&(bb_cache_update_t){ .key = t->name, .snap = t->initial_snap });
        TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, err, t->name);
    }

    // Serialize twice, independently
    bb_json_t obj_a = bb_json_obj_new();
    bb_json_t obj_b = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj_a);
    TEST_ASSERT_NOT_NULL(obj_b);

    err = bb_cache_serialize_into(t->name, obj_a);
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, err, t->name);

    err = bb_cache_serialize_into(t->name, obj_b);
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, err, t->name);

    char *json_a = bb_json_serialize(obj_a);
    char *json_b = bb_json_serialize(obj_b);
    TEST_ASSERT_NOT_NULL(json_a);
    TEST_ASSERT_NOT_NULL(json_b);

    TEST_ASSERT_EQUAL_STRING_MESSAGE(json_a, json_b, t->name);

    bb_json_free_str(json_a);
    bb_json_free_str(json_b);
    bb_json_free(obj_a);
    bb_json_free(obj_b);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Iterates every topic in the table and checks event == REST output.
void test_bb_cache_fidelity_all_topics(void)
{
    reset_all();
    for (int i = 0; i < N_TOPICS; i++) {
        assert_fidelity(&s_topics[i]);
    }
}

// Verify register is idempotent (second call is a no-op, no duplicate entries).
void test_bb_cache_register_idempotent(void)
{
    reset_all();
    bb_err_t err;
    err = reg("test.synth", NULL, sizeof(synth_snap_t), synth_serialize);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    err = reg("test.synth", NULL, sizeof(synth_snap_t), synth_serialize);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

// Verify update mutates the owned struct and is reflected in serialize_into.
void test_bb_cache_update_reflected_in_serialize(void)
{
    reset_all();
    reg("test.synth", NULL, sizeof(synth_snap_t), synth_serialize);

    synth_snap_t s1 = {.value = 1, .flag = false, .ratio = 0.1};
    bb_cache_update(&(bb_cache_update_t){ .key = "test.synth", .snap = &s1 });
    bb_json_t obj1 = bb_json_obj_new();
    bb_cache_serialize_into("test.synth", obj1);
    char *j1 = bb_json_serialize(obj1);

    synth_snap_t s2 = {.value = 99, .flag = true, .ratio = 3.14};
    bb_cache_update(&(bb_cache_update_t){ .key = "test.synth", .snap = &s2 });
    bb_json_t obj2 = bb_json_obj_new();
    bb_cache_serialize_into("test.synth", obj2);
    char *j2 = bb_json_serialize(obj2);

    // Different values must produce different JSON.
    TEST_ASSERT_NOT_EQUAL(0, strcmp(j1, j2));

    // Re-serialize after second update must still match.
    bb_json_t obj3 = bb_json_obj_new();
    bb_cache_serialize_into("test.synth", obj3);
    char *j3 = bb_json_serialize(obj3);
    TEST_ASSERT_EQUAL_STRING(j2, j3);

    bb_json_free_str(j1);
    bb_json_free_str(j2);
    bb_json_free_str(j3);
    bb_json_free(obj1);
    bb_json_free(obj2);
    bb_json_free(obj3);
}

// Verify serialize_into on an unknown topic returns BB_ERR_NOT_FOUND.
void test_bb_cache_serialize_into_unknown_topic(void)
{
    reset_all();
    bb_json_t obj = bb_json_obj_new();
    bb_err_t err = bb_cache_serialize_into("no.such.topic", obj);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, err);
    bb_json_free(obj);
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
        bb_err_t err = reg(key_buf, NULL, sizeof(synth_snap_t), synth_serialize);
        TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, err, "fill should succeed");
    }
    // One more must fail
    bb_err_t err = reg("test.fill.overflow", NULL,
                       sizeof(synth_snap_t), synth_serialize);
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

    synth_snap_t s = {.value = 1, .flag = false, .ratio = 0.0};
    reg("test.enum.a", NULL, sizeof(synth_snap_t), synth_serialize);
    bb_cache_update(&(bb_cache_update_t){ .key = "test.enum.a", .snap = &s });
    reg("test.enum.b", NULL, sizeof(synth_snap_t), synth_serialize);
    bb_cache_update(&(bb_cache_update_t){ .key = "test.enum.b", .snap = &s });
    reg("test.enum.c", NULL, sizeof(synth_snap_t), synth_serialize);
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
    reg("test.enum.only", NULL, sizeof(synth_snap_t), synth_serialize);

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
    reg("test.enum.longkey", NULL, sizeof(synth_snap_t), synth_serialize);

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
    reg("test.raw", NULL, sizeof(synth_snap_t), synth_serialize);

    synth_snap_t in = {.value = 7, .flag = true, .ratio = 2.5};
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

static synth_snap_t s_getter_backing = {.value = 5, .flag = true, .ratio = 1.0};
static const void *get_raw_test_getter(void) { return &s_getter_backing; }

// Verify bb_cache_get_raw on a getter-mode topic returns BB_ERR_INVALID_STATE.
void test_bb_cache_get_raw_getter_mode_returns_invalid_state(void)
{
    reset_all();
    reg("test.raw.getter", get_raw_test_getter, 0, synth_serialize);

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
    reg("test.raw.args", NULL, sizeof(synth_snap_t), synth_serialize);
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
// Regression test for a torn-read race: bb_cache_update/post/serialize_into/
// post_serialized/get_serialized used to scan s_entries[] via a raw,
// unlocked find_entry() while bb_cache_register() concurrently wrote new
// slot fields under s_reg_lock. This test hammers a stable, already-
// registered topic with lookups (bb_cache_get_serialized) from one thread
// while a second thread concurrently registers a burst of new topics --
// exercising exactly the interleaving the fix (find_entry_locked) guards
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
        bb_err_t err = reg(s_b1_568_names[i], NULL, sizeof(synth_snap_t), synth_serialize);
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
    char buf[256];
    for (int i = 0; i < B1_568_BURST_TOPICS; i++) {
        size_t out_len = 0;
        bb_err_t err = bb_cache_get_serialized("test.b1568.stable", buf, sizeof(buf), &out_len);
        if (err != BB_OK) {
            res->lookup_fail_count++;
            continue;
        }
        if (out_len == 0 || strstr(buf, "\"value\":42") == NULL) {
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
    bb_err_t err = reg("test.b1568.stable", NULL, sizeof(synth_snap_t), synth_serialize);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    synth_snap_t stable = {.value = 42, .flag = true, .ratio = 1.0};
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
        .serialize = synth_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    bb_err_t err = bb_cache_register(&cfg);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    synth_snap_t s = {.value = 5, .flag = true, .ratio = 1.5};
    err = bb_cache_update(&(bb_cache_update_t){ .key = "test.cfg.basic", .snap = &s });
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    bb_json_t obj = bb_json_obj_new();
    err = bb_cache_serialize_into("test.cfg.basic", obj);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    char *json = bb_json_serialize(obj);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"value\":5"));
    bb_json_free_str(json);
    bb_json_free(obj);
}

// Verify bb_cache_register rejects a NULL cfg, NULL cfg->key, and NULL
// cfg->serialize.
void test_bb_cache_register_config_struct_null_args(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_register(NULL));

    bb_cache_config_t no_key = {
        .key = NULL, .snapshot = NULL, .snap_size = sizeof(synth_snap_t),
        .serialize = synth_serialize, .flags = BB_CACHE_FLAG_NONE,
    };
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_register(&no_key));

    bb_cache_config_t no_serialize = {
        .key = "test.cfg.nokey", .snapshot = NULL, .snap_size = sizeof(synth_snap_t),
        .serialize = NULL, .flags = BB_CACHE_FLAG_NONE,
    };
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_register(&no_serialize));
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
        .serialize = synth_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    bb_err_t err = bb_cache_register(&cfg);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    // Free the caller's key buffer immediately — bb_cache must have copied it.
    memset(key, 0xAA, 32);
    free(key);
    key = NULL;

    synth_snap_t s = {.value = 9, .flag = false, .ratio = 0.0};
    err = bb_cache_update(&(bb_cache_update_t){ .key = "test.uaf.key", .snap = &s });
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, err, "lookup by original key value must survive freeing the caller's buffer");

    bb_json_t obj = bb_json_obj_new();
    err = bb_cache_serialize_into("test.uaf.key", obj);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    char *json = bb_json_serialize(obj);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"value\":9"));
    bb_json_free_str(json);
    bb_json_free(obj);
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
        .serialize = synth_serialize,
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
        .serialize = synth_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    bb_err_t err = bb_cache_register(&cfg);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_UINT(1, bb_cache_count());
}

// SSE-flag default preservation: a config struct with flags left at
// BB_CACHE_FLAG_NONE (zero-init, matching a caller that forgets to migrate
// the old always-SSE legacy default) must NOT get an SSE event topic —
// bb_cache_post returns BB_ERR_INVALID_STATE, not BB_OK.
void test_bb_cache_register_zero_flags_has_no_sse(void)
{
    reset_all();
    bb_cache_config_t cfg = {
        .key       = "test.cfg.noflags",
        .snapshot  = NULL,
        .snap_size = sizeof(synth_snap_t),
        .serialize = synth_serialize,
        .flags     = 0, // BB_CACHE_FLAG_NONE, explicit zero-init
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));

    synth_snap_t s = {.value = 1, .flag = false, .ratio = 0.0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.cfg.noflags", .snap = &s }));

    bb_err_t err = bb_cache_post("test.cfg.noflags");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, err);
}

// SSE-flag default preservation: a migrated caller that explicitly sets
// .flags = BB_CACHE_FLAG_SSE gets identical behaviour to the old always-SSE
// legacy bb_cache_register() — bb_cache_post succeeds.
void test_bb_cache_register_explicit_sse_flag_preserves_legacy_behavior(void)
{
    reset_all();
    bb_event_init(NULL);
    bb_cache_config_t cfg = {
        .key       = "test.cfg.sseflag",
        .snapshot  = NULL,
        .snap_size = sizeof(synth_snap_t),
        .serialize = synth_serialize,
        .flags     = BB_CACHE_FLAG_SSE,
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));

    synth_snap_t s = {.value = 2, .flag = true, .ratio = 0.5};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.cfg.sseflag", .snap = &s }));

    bb_err_t err = bb_cache_post("test.cfg.sseflag");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

// ---------------------------------------------------------------------------
// Envelope contract (B1-570 PR-3): bb_cache_get_serialized/bb_cache_post wrap
// the serializer's output as {"ts_ms":<n>,"data":{...}}. bb_cache_serialize_into
// stays raw (embed-a-section primitive, no envelope).
// ---------------------------------------------------------------------------

// Small SSE capture helper — subscribes to a key's event topic, posts, drains
// synchronously (BB_EVENT_HOST_SYNC), and hands back the raw payload bytes.
static char   s_env_sse_payload[512];
static size_t s_env_sse_len;
static int    s_env_sse_calls;

static void env_sse_capture_cb(bb_event_topic_t topic, int32_t id,
                                const void *data, size_t size, void *user)
{
    (void)topic; (void)id; (void)user;
    s_env_sse_calls++;
    if (data && size > 0) {
        size_t n = size - 1;  // strip NUL posted by bb_cache_post
        if (n >= sizeof(s_env_sse_payload)) n = sizeof(s_env_sse_payload) - 1;
        memcpy(s_env_sse_payload, data, n);
        s_env_sse_payload[n] = '\0';
        s_env_sse_len = n;
    }
}

void test_bb_cache_envelope_get_serialized_owned_mode_shape(void)
{
    reset_all();

    synth_snap_t s = {.value = 7, .flag = true, .ratio = 1.5};
    TEST_ASSERT_EQUAL_INT(BB_OK, reg("test.env.owned", NULL, sizeof(s), synth_serialize));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.env.owned", .snap = &s }));

    char buf[256];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("test.env.owned", buf, sizeof(buf), &len));

    bb_json_t root = bb_json_parse(buf, len);
    TEST_ASSERT_NOT_NULL(root);

    double ts = -1;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(root, "ts_ms", &ts));
    TEST_ASSERT_TRUE(ts > 0);

    bb_json_t data = bb_json_obj_get_item(root, "data");
    TEST_ASSERT_NOT_NULL(data);
    double value = 0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(data, "value", &value));
    TEST_ASSERT_EQUAL_INT(7, (int)value);

    // "value" must NOT also be at the envelope root (no flat/nested dup).
    double flat = 0;
    TEST_ASSERT_FALSE(bb_json_obj_get_number(root, "value", &flat));

    bb_json_free(root);
}

void test_bb_cache_envelope_owned_mode_ts_frozen_between_reads(void)
{
    reset_all();

    synth_snap_t s = {.value = 1, .flag = false, .ratio = 0.0};
    TEST_ASSERT_EQUAL_INT(BB_OK, reg("test.env.frozen", NULL, sizeof(s), synth_serialize));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.env.frozen", .snap = &s }));

    char buf1[256], buf2[256];
    size_t len1 = 0, len2 = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("test.env.frozen", buf1, sizeof(buf1), &len1));
    usleep(2000);  // clear any clock-resolution ambiguity
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("test.env.frozen", buf2, sizeof(buf2), &len2));

    // Owned mode: ts_ms is stamped at bb_cache_update(), not at read time —
    // two reads with no update in between must be byte-identical.
    TEST_ASSERT_EQUAL_UINT(len1, len2);
    TEST_ASSERT_EQUAL_STRING(buf1, buf2);
}

void test_bb_cache_envelope_owned_mode_ts_advances_on_update(void)
{
    reset_all();

    synth_snap_t s = {.value = 1, .flag = false, .ratio = 0.0};
    TEST_ASSERT_EQUAL_INT(BB_OK, reg("test.env.advance", NULL, sizeof(s), synth_serialize));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.env.advance", .snap = &s }));

    char buf1[256];
    size_t len1 = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("test.env.advance", buf1, sizeof(buf1), &len1));
    bb_json_t root1 = bb_json_parse(buf1, len1);
    double ts1 = -1;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(root1, "ts_ms", &ts1));
    bb_json_free(root1);

    usleep(2000);
    s.value = 2;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.env.advance", .snap = &s }));

    char buf2[256];
    size_t len2 = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("test.env.advance", buf2, sizeof(buf2), &len2));
    bb_json_t root2 = bb_json_parse(buf2, len2);
    double ts2 = -1;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(root2, "ts_ms", &ts2));
    bb_json_free(root2);

    TEST_ASSERT_TRUE(ts2 >= ts1);
}

static synth_snap_t s_env_getter_val = {.value = 3, .flag = true, .ratio = 2.0};

static const void *env_getter_snapshot(void)
{
    return &s_env_getter_val;
}

void test_bb_cache_envelope_getter_mode_ts_advances_each_read(void)
{
    reset_all();

    TEST_ASSERT_EQUAL_INT(BB_OK,
        reg("test.env.getter", env_getter_snapshot, 0, synth_serialize));

    char buf1[256];
    size_t len1 = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("test.env.getter", buf1, sizeof(buf1), &len1));
    bb_json_t root1 = bb_json_parse(buf1, len1);
    double ts1 = -1;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(root1, "ts_ms", &ts1));
    bb_json_free(root1);

    usleep(2000);

    char buf2[256];
    size_t len2 = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("test.env.getter", buf2, sizeof(buf2), &len2));
    bb_json_t root2 = bb_json_parse(buf2, len2);
    double ts2 = -1;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(root2, "ts_ms", &ts2));
    bb_json_free(root2);

    // Getter mode: every call re-samples snapshot() AND re-stamps ts_ms
    // (the read IS the sample) — the second read must be no earlier.
    TEST_ASSERT_TRUE(ts2 >= ts1);
}

void test_bb_cache_envelope_post_sse_shape(void)
{
    reset_all();
    s_env_sse_calls = 0;
    s_env_sse_len   = 0;
    s_env_sse_payload[0] = '\0';

    synth_snap_t s = {.value = 9, .flag = false, .ratio = 0.25};
    bb_cache_config_t cfg = {
        .key       = "test.env.post",
        .snapshot  = NULL,
        .snap_size = sizeof(s),
        .serialize = synth_serialize,
        .flags     = BB_CACHE_FLAG_SSE,
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.env.post", .snap = &s }));

    bb_event_topic_t topic = NULL;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_event_topic_lookup("test.env.post", &topic));
    bb_event_sub_t sub = NULL;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_event_subscribe(topic, env_sse_capture_cb, NULL, &sub));

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_post("test.env.post"));
    bb_event_pump(0);
    TEST_ASSERT_EQUAL_INT(1, s_env_sse_calls);

    bb_json_t root = bb_json_parse(s_env_sse_payload, s_env_sse_len);
    TEST_ASSERT_NOT_NULL(root);
    double ts = -1;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(root, "ts_ms", &ts));
    bb_json_t data = bb_json_obj_get_item(root, "data");
    TEST_ASSERT_NOT_NULL(data);
    double value = 0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(data, "value", &value));
    TEST_ASSERT_EQUAL_INT(9, (int)value);

    bb_json_free(root);
    bb_event_unsubscribe(sub);
}

void test_bb_cache_envelope_rest_equals_sse_within_interval(void)
{
    // REST (bb_cache_get_serialized) and SSE (bb_cache_post) must produce the
    // SAME {"ts_ms":n,"data":{...}} bytes when no update() lands in between —
    // both are owned-mode reads of the same frozen (struct, ts_ms) pair.
    reset_all();
    s_env_sse_calls = 0;
    s_env_sse_len   = 0;
    s_env_sse_payload[0] = '\0';

    synth_snap_t s = {.value = 11, .flag = true, .ratio = 3.5};
    bb_cache_config_t cfg = {
        .key       = "test.env.parity",
        .snapshot  = NULL,
        .snap_size = sizeof(s),
        .serialize = synth_serialize,
        .flags     = BB_CACHE_FLAG_SSE,
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.env.parity", .snap = &s }));

    bb_event_topic_t topic = NULL;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_event_topic_lookup("test.env.parity", &topic));
    bb_event_sub_t sub = NULL;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_event_subscribe(topic, env_sse_capture_cb, NULL, &sub));

    char rest_buf[256];
    size_t rest_len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("test.env.parity", rest_buf, sizeof(rest_buf), &rest_len));

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_post("test.env.parity"));
    bb_event_pump(0);
    TEST_ASSERT_EQUAL_INT(1, s_env_sse_calls);

    TEST_ASSERT_EQUAL_UINT(rest_len, s_env_sse_len);
    TEST_ASSERT_EQUAL_STRING(rest_buf, s_env_sse_payload);

    bb_event_unsubscribe(sub);
}

void test_bb_cache_envelope_serialize_into_not_enveloped(void)
{
    // bb_cache_serialize_into is the embed-a-section primitive — it must
    // NEVER apply the {"ts_ms","data"} wrapper (that would double-nest a
    // composed document, e.g. /api/health aggregating multiple keys).
    reset_all();

    synth_snap_t s = {.value = 5, .flag = true, .ratio = 1.0};
    TEST_ASSERT_EQUAL_INT(BB_OK, reg("test.env.raw", NULL, sizeof(s), synth_serialize));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.env.raw", .snap = &s }));

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_serialize_into("test.env.raw", obj));

    double value = 0, flat_ts = 0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(obj, "value", &value));
    TEST_ASSERT_EQUAL_INT(5, (int)value);
    TEST_ASSERT_FALSE(bb_json_obj_get_number(obj, "ts_ms", &flat_ts));
    TEST_ASSERT_NULL(bb_json_obj_get_item(obj, "data"));

    bb_json_free(obj);
}

// ---------------------------------------------------------------------------
// B1-... PR-4a: bb_cache_update's out_changed change-detection + bb_cache_exists
// ---------------------------------------------------------------------------

// First write since register must report changed=true, even against a
// zero-initialized owned buffer (has_value guard, no false negative).
void test_bb_cache_update_out_changed_first_write_all_zero_reports_changed(void)
{
    reset_all();
    reg("test.ex.firstzero", NULL, sizeof(synth_snap_t), synth_serialize);

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
    reg("test.ex.identical", NULL, sizeof(synth_snap_t), synth_serialize);

    synth_snap_t s = {.value = 1, .flag = true, .ratio = 0.5};
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
    reg("test.ex.diff", NULL, sizeof(synth_snap_t), synth_serialize);

    synth_snap_t s1 = {.value = 1, .flag = false, .ratio = 0.0};
    bool changed = false;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.ex.diff", .snap = &s1, .out_changed = &changed }));
    TEST_ASSERT_TRUE(changed);

    synth_snap_t s2 = {.value = 2, .flag = false, .ratio = 0.0};
    changed = false;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.ex.diff", .snap = &s2, .out_changed = &changed }));
    TEST_ASSERT_TRUE(changed);
}

// out_changed == NULL must not crash and must still copy in the value.
void test_bb_cache_update_null_out_changed_does_not_crash(void)
{
    reset_all();
    reg("test.ex.nullout", NULL, sizeof(synth_snap_t), synth_serialize);

    synth_snap_t s = {.value = 3, .flag = true, .ratio = 1.0};
    bb_err_t err = bb_cache_update(&(bb_cache_update_t){ .key = "test.ex.nullout", .snap = &s });
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_serialize_into("test.ex.nullout", obj));
    double value = 0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(obj, "value", &value));
    TEST_ASSERT_EQUAL_INT(3, (int)value);
    bb_json_free(obj);
}

// Getter-mode keys report changed=false always (no owned bytes to diff).
void test_bb_cache_update_getter_mode_out_changed_reports_unchanged(void)
{
    reset_all();
    reg("test.ex.getter", env_getter_snapshot, 0, synth_serialize);

    synth_snap_t s = {.value = 4, .flag = false, .ratio = 0.0};
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
    reg("test.ex.getter.nullout", env_getter_snapshot, 0, synth_serialize);

    synth_snap_t s = {.value = 4, .flag = false, .ratio = 0.0};
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

// ts_ms == 0 (the default, zero-init struct field) stamps the envelope
// sample-time as "now" -- unchanged default behavior for every existing
// caller that never sets .ts_ms.
void test_bb_cache_update_ts_ms_zero_stamps_now(void)
{
    reset_all();
    reg("test.tsms.zero", NULL, sizeof(synth_snap_t), synth_serialize);

    synth_snap_t s = {.value = 1, .flag = false, .ratio = 0.0};
    int64_t before = (int64_t)bb_clock_now_ms64();
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_update(&(bb_cache_update_t){ .key = "test.tsms.zero", .snap = &s }));
    int64_t after = (int64_t)bb_clock_now_ms64();

    char buf[256];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("test.tsms.zero", buf, sizeof(buf), &len));
    bb_json_t root = bb_json_parse(buf, len);
    double ts = -1;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(root, "ts_ms", &ts));
    bb_json_free(root);

    TEST_ASSERT_TRUE((int64_t)ts >= before);
    TEST_ASSERT_TRUE((int64_t)ts <= after);
}

// A nonzero .ts_ms overrides the "now" stamp with the caller-supplied sample
// time (e.g. an ingress source's own timestamp).
void test_bb_cache_update_ts_ms_nonzero_overrides_stamp(void)
{
    reset_all();
    reg("test.tsms.override", NULL, sizeof(synth_snap_t), synth_serialize);

    synth_snap_t s = {.value = 1, .flag = false, .ratio = 0.0};
    const int64_t custom_ts = 123456789;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_update(&(bb_cache_update_t){
            .key = "test.tsms.override", .snap = &s, .ts_ms = custom_ts }));

    char buf[256];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("test.tsms.override", buf, sizeof(buf), &len));
    bb_json_t root = bb_json_parse(buf, len);
    double ts = -1;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(root, "ts_ms", &ts));
    bb_json_free(root);

    TEST_ASSERT_EQUAL_INT64(custom_ts, (int64_t)ts);
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
    reg("test.ex.wrapper", NULL, sizeof(synth_snap_t), synth_serialize);

    synth_snap_t s = {.value = 6, .flag = true, .ratio = 2.0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.ex.wrapper", .snap = &s }));

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_serialize_into("test.ex.wrapper", obj));
    double value = 0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(obj, "value", &value));
    TEST_ASSERT_EQUAL_INT(6, (int)value);
    bb_json_free(obj);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_cache_update(&(bb_cache_update_t){ .key = "no.such.ex.topic", .snap = &s }));
}

// bb_cache_exists: true for a registered key, false for an unregistered key.
void test_bb_cache_exists_registered_and_unregistered(void)
{
    reset_all();
    TEST_ASSERT_FALSE(bb_cache_exists("test.ex.exists"));

    reg("test.ex.exists", NULL, sizeof(synth_snap_t), synth_serialize);
    TEST_ASSERT_TRUE(bb_cache_exists("test.ex.exists"));

    TEST_ASSERT_FALSE(bb_cache_exists("no.such.ex.exists.topic"));
}

void test_bb_cache_envelope_get_serialized_undersized_buffer_untouched(void)
{
    // Buffer sizing: the envelope adds fixed overhead on top of the memoized
    // "data" bytes. A cap that fits "data" alone but not the full envelope
    // must be refused (BB_ERR_NO_SPACE) WITHOUT a partial write.
    reset_all();

    synth_snap_t s = {.value = 1, .flag = false, .ratio = 0.0};
    TEST_ASSERT_EQUAL_INT(BB_OK, reg("test.env.undersized", NULL, sizeof(s), synth_serialize));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.env.undersized", .snap = &s }));

    // First, learn the raw "data" length via a big-enough buffer.
    char big[256];
    size_t big_len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("test.env.undersized", big, sizeof(big), &big_len));

    // A cap 1 byte short of the full envelope must fail cleanly.
    char small[256];
    memset(small, 'Z', sizeof(small));
    size_t small_cap = big_len;  // < needed (needed == big_len + 1 for the NUL)
    bb_err_t err = bb_cache_get_serialized("test.env.undersized", small, small_cap, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_EQUAL_CHAR('Z', small[0]);  // buffer untouched
}

// ---------------------------------------------------------------------------
// PR-4a-0: owned+fallback cold-start seed (bb_cache.h tri-state, third combo:
// snapshot != NULL AND snap_size > 0). Covers: seed-on-first-read, seed
// invoked at most once, a real write always wins and never re-triggers the
// seed, the seed does NOT count toward bb_cache_update's out_changed/has_value
// guard, a fallback getter returning NULL leaves the owned buffer untouched
// (zero-initialized) without retrying, and every read/serialize entry point
// (bb_cache_get_serialized, bb_cache_serialize_into, bb_cache_get_raw) seeds
// consistently. Note: bb_cache core (PR-4a) has no observer/notify machinery
// yet (that lands in PR-4b) -- "the seed does not fire observers" is
// trivially true today; there is nothing to subscribe to.
// ---------------------------------------------------------------------------

static synth_snap_t s_cs_fallback_val = {.value = 77, .flag = true, .ratio = 4.25};
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
        reg("test.cs.seed", cs_fallback_snapshot, sizeof(synth_snap_t), synth_serialize));

    char buf[256];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("test.cs.seed", buf, sizeof(buf), &len));

    bb_json_t root = bb_json_parse(buf, len);
    TEST_ASSERT_NOT_NULL(root);
    bb_json_t data = bb_json_obj_get_item(root, "data");
    TEST_ASSERT_NOT_NULL(data);
    double value = 0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(data, "value", &value));
    TEST_ASSERT_EQUAL_INT(77, (int)value);
    bb_json_free(root);

    TEST_ASSERT_EQUAL_INT(1, s_cs_snapshot_calls);
}

// The seed fires at most ONCE while unpopulated -- repeated reads before any
// real write must not re-invoke the fallback getter. Uses
// bb_cache_get_serialized, which additionally memoizes via the dirty flag
// (a second, independent reason the getter isn't re-invoked on reads 2/3).
void test_bb_cache_owned_fallback_seed_invoked_once_across_reads(void)
{
    reset_all();
    cs_reset();

    reg("test.cs.once", cs_fallback_snapshot, sizeof(synth_snap_t), synth_serialize);

    char buf[256];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("test.cs.once", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("test.cs.once", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("test.cs.once", buf, sizeof(buf), &len));

    TEST_ASSERT_EQUAL_INT(1, s_cs_snapshot_calls);
}

// maybe_seed_fallback's OWN idempotency guard (fallback_seeded), isolated
// from bb_cache_get_serialized's separate dirty-flag memoization:
// bb_cache_serialize_into has no dirty gate at all -- it calls
// serialize_locked (and therefore maybe_seed_fallback) on EVERY call. Two
// back-to-back calls on an unpopulated owned+fallback entry must still only
// invoke the fallback getter once -- the second call observes
// fallback_seeded==true (while has_value is still false, no real write yet)
// and skips re-invoking it.
void test_bb_cache_owned_fallback_serialize_into_seed_not_reinvoked_without_dirty_gate(void)
{
    reset_all();
    cs_reset();

    reg("test.cs.reinvoke", cs_fallback_snapshot, sizeof(synth_snap_t), synth_serialize);

    bb_json_t obj1 = bb_json_obj_new();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_serialize_into("test.cs.reinvoke", obj1));
    bb_json_free(obj1);
    TEST_ASSERT_EQUAL_INT(1, s_cs_snapshot_calls);

    bb_json_t obj2 = bb_json_obj_new();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_serialize_into("test.cs.reinvoke", obj2));
    double value = 0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(obj2, "value", &value));
    TEST_ASSERT_EQUAL_INT(77, (int)value);  // still the seeded value
    bb_json_free(obj2);

    TEST_ASSERT_EQUAL_INT(1, s_cs_snapshot_calls);
}

// Plain GETTER mode (owned == NULL) reached via bb_cache_serialize_into and
// bb_cache_post -- the two entry points that share serialize_locked with the
// owned+fallback path above. Exercises the "e->snapshot && !e->owned" TRUE
// branch (pure pull-through, no owned buffer, no seeding) in serialize_locked
// specifically, as distinct from bb_cache_get_serialized's own copy of that
// same branch (covered elsewhere in this file).
void test_bb_cache_getter_mode_serialize_into_and_post_pull_through(void)
{
    reset_all();
    bb_event_init(NULL);

    bb_cache_config_t cfg = {
        .key       = "test.cs.getterpost",
        .snapshot  = env_getter_snapshot,
        .snap_size = 0,
        .serialize = synth_serialize,
        .flags     = BB_CACHE_FLAG_SSE,
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_serialize_into("test.cs.getterpost", obj));
    double value = 0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(obj, "value", &value));
    TEST_ASSERT_EQUAL_INT(3, (int)value);  // s_env_getter_val.value
    bb_json_free(obj);

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_post("test.cs.getterpost"));
}

// The cold-start seed is a read-side effect only -- it must never fire an
// SSE observer/event. Subscribes to the owned+fallback key's event topic,
// triggers the seed via a plain read (bb_cache_get_serialized), and asserts
// the subscriber callback never runs. (bb_cache core has no reactive/notify
// layer yet -- PR-4b -- so this pins the only observer-like mechanism that
// exists today: BB_CACHE_FLAG_SSE's bb_event topic, which only bb_cache_post
// ever posts to.)
void test_bb_cache_owned_fallback_seed_does_not_fire_sse_observer(void)
{
    reset_all();
    s_env_sse_calls = 0;

    bb_cache_config_t cfg = {
        .key       = "test.cs.noobserve",
        .snapshot  = cs_fallback_snapshot,
        .snap_size = sizeof(synth_snap_t),
        .serialize = synth_serialize,
        .flags     = BB_CACHE_FLAG_SSE,
    };
    cs_reset();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));

    bb_event_topic_t topic = NULL;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_event_topic_lookup("test.cs.noobserve", &topic));
    bb_event_sub_t sub = NULL;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_event_subscribe(topic, env_sse_capture_cb, NULL, &sub));

    char buf[256];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("test.cs.noobserve", buf, sizeof(buf), &len));  // seeds
    bb_event_pump(0);

    TEST_ASSERT_EQUAL_INT(1, s_cs_snapshot_calls);
    TEST_ASSERT_EQUAL_INT(0, s_env_sse_calls);  // the seed never posted an event

    bb_event_unsubscribe(sub);
}

// A real bb_cache_update always wins and permanently stops the seed path
// (strict boot bridge -- no expiry, never re-runs after a real write).
void test_bb_cache_owned_fallback_real_write_wins_and_stops_seeding(void)
{
    reset_all();
    cs_reset();

    reg("test.cs.write", cs_fallback_snapshot, sizeof(synth_snap_t), synth_serialize);

    // Trigger the seed via a read before any real write.
    char buf[256];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("test.cs.write", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_INT(1, s_cs_snapshot_calls);

    synth_snap_t written = {.value = 123, .flag = false, .ratio = 9.5};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.cs.write", .snap = &written }));

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("test.cs.write", buf, sizeof(buf), &len));
    bb_json_t root = bb_json_parse(buf, len);
    bb_json_t data = bb_json_obj_get_item(root, "data");
    double value = 0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(data, "value", &value));
    TEST_ASSERT_EQUAL_INT(123, (int)value);
    bb_json_free(root);

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

    reg("test.cs.changed", cs_fallback_snapshot, sizeof(synth_snap_t), synth_serialize);

    char buf[256];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("test.cs.changed", buf, sizeof(buf), &len));  // seeds

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

    reg("test.cs.nullsnap", cs_fallback_snapshot, sizeof(synth_snap_t), synth_serialize);

    char buf[256];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("test.cs.nullsnap", buf, sizeof(buf), &len));
    bb_json_t root = bb_json_parse(buf, len);
    bb_json_t data = bb_json_obj_get_item(root, "data");
    double value = -1;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(data, "value", &value));
    TEST_ASSERT_EQUAL_INT(0, (int)value);  // owned buffer stayed zero-initialized
    bb_json_free(root);
    TEST_ASSERT_EQUAL_INT(1, s_cs_snapshot_calls);

    // Second read must not retry the failed seed.
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("test.cs.nullsnap", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_INT(1, s_cs_snapshot_calls);
}

// bb_cache_get_raw (the compact struct-read path) also seeds when
// unpopulated -- consistent with the JSON read paths above.
void test_bb_cache_owned_fallback_get_raw_seeds_when_unpopulated(void)
{
    reset_all();
    cs_reset();

    reg("test.cs.raw", cs_fallback_snapshot, sizeof(synth_snap_t), synth_serialize);

    synth_snap_t out = {0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_raw("test.cs.raw", &out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(77, out.value);
    TEST_ASSERT_EQUAL_INT(1, s_cs_snapshot_calls);
}

// bb_cache_serialize_into (the embed-a-section path) also seeds when
// unpopulated -- consistent with the enveloped read paths above.
void test_bb_cache_owned_fallback_serialize_into_seeds(void)
{
    reset_all();
    cs_reset();

    reg("test.cs.serinto", cs_fallback_snapshot, sizeof(synth_snap_t), synth_serialize);

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_serialize_into("test.cs.serinto", obj));
    double value = 0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(obj, "value", &value));
    TEST_ASSERT_EQUAL_INT(77, (int)value);
    bb_json_free(obj);
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
        .serialize = synth_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_register(&cfg));
    TEST_ASSERT_FALSE(bb_cache_exists("test.cs.needsize"));
}

// ---------------------------------------------------------------------------
// B1-592 PR-A1: bb_cache_delete (runtime eviction primitive)
// ---------------------------------------------------------------------------

// Deleting an existing key removes it from the registry: exists() flips to
// false and get_serialized returns BB_ERR_NOT_FOUND.
void test_bb_cache_delete_existing_removes_entry(void)
{
    reset_all();
    reg("test.del.basic", NULL, sizeof(synth_snap_t), synth_serialize);
    synth_snap_t s = {.value = 1, .flag = true, .ratio = 0.5};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.del.basic", .snap = &s }));
    TEST_ASSERT_TRUE(bb_cache_exists("test.del.basic"));

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_delete("test.del.basic"));

    TEST_ASSERT_FALSE(bb_cache_exists("test.del.basic"));
    char buf[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND,
        bb_cache_get_serialized("test.del.basic", buf, sizeof(buf), &len));
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
// fresh, never-used slot: has_value=false (no stale change-detect memory) and
// no leftover cached_json from the deleted incarnation. Compared bytewise
// against a topic that was registered fresh and never touched.
void test_bb_cache_delete_then_reregister_same_key_is_fresh(void)
{
    reset_all();

    // Register, write, and force a memoized serialize on the FIRST incarnation.
    reg("test.del.reuse", NULL, sizeof(synth_snap_t), synth_serialize);
    synth_snap_t s1 = {.value = 111, .flag = true, .ratio = 9.5};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.del.reuse", .snap = &s1 }));
    char scratch[256];
    size_t scratch_len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("test.del.reuse", scratch, sizeof(scratch), &scratch_len));

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_delete("test.del.reuse"));

    // Re-register the SAME key -- second incarnation.
    TEST_ASSERT_EQUAL_INT(BB_OK,
        reg("test.del.reuse", NULL, sizeof(synth_snap_t), synth_serialize));

    // A control topic, registered fresh and never touched, for the bytewise
    // comparison below.
    TEST_ASSERT_EQUAL_INT(BB_OK,
        reg("test.del.control", NULL, sizeof(synth_snap_t), synth_serialize));

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

    // Serialized output must match the control topic bytewise -- no stale
    // cached_json bleeding through from the deleted first incarnation.
    char reused_buf[256], control_buf[256];
    size_t reused_len = 0, control_len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("test.del.reuse", reused_buf, sizeof(reused_buf), &reused_len));
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("test.del.control", control_buf, sizeof(control_buf), &control_len));

    // Strip the ts_ms envelope field (timestamps may legitimately differ by a
    // tick) and compare only the "data" payload bytes.
    bb_json_t reused_root = bb_json_parse(reused_buf, reused_len);
    bb_json_t control_root = bb_json_parse(control_buf, control_len);
    TEST_ASSERT_NOT_NULL(reused_root);
    TEST_ASSERT_NOT_NULL(control_root);
    char *reused_data = bb_json_serialize(bb_json_obj_get_item(reused_root, "data"));
    char *control_data = bb_json_serialize(bb_json_obj_get_item(control_root, "data"));
    TEST_ASSERT_EQUAL_STRING(control_data, reused_data);

    bb_json_free_str(reused_data);
    bb_json_free_str(control_data);
    bb_json_free(reused_root);
    bb_json_free(control_root);
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

    reg("test.del.foreach.victim", NULL, sizeof(synth_snap_t), synth_serialize);

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
        TEST_ASSERT_EQUAL_INT(BB_OK, reg(key_buf, NULL, sizeof(synth_snap_t), synth_serialize));
    }

    // Delete one slot, then refill it -- table is full again.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_delete("test.del.fill.0"));
    TEST_ASSERT_EQUAL_INT(BB_OK, reg("test.del.fill.0", NULL, sizeof(synth_snap_t), synth_serialize));

    bb_err_t err = reg("test.del.fill.overflow", NULL, sizeof(synth_snap_t), synth_serialize);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
}

// Deleting a getter-mode entry (owned == NULL) must be a clean no-crash path
// through free_entry_locked's bb_mem_free(e->owned) call -- bb_mem_free(NULL)
// is required to be a safe no-op, and getter-mode entries never allocate an
// owned buffer.
void test_bb_cache_delete_getter_mode_entry_no_crash(void)
{
    reset_all();
    reg("test.del.getter", get_raw_test_getter, 0, synth_serialize);
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
        .serialize = synth_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    bb_err_t err = bb_cache_register(&cfg);
    bb_mem_set_alloc_hook(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_FALSE(bb_cache_exists("test.cs.allocfail"));
}

void test_bb_cache_post_null_key_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_post(NULL));
}

void test_bb_cache_post_unknown_key_returns_not_found(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_cache_post("no.such.post.key"));
}

// serialize_locked returning non-BB_OK from inside bb_cache_post: a getter
// with no snapshot data available yields BB_ERR_INVALID_STATE.
static const void *post_null_getter(void) { return NULL; }

void test_bb_cache_post_serialize_locked_error_propagates(void)
{
    reset_all();
    bb_event_init(NULL);
    bb_cache_config_t cfg = {
        .key       = "test.post.nosnap",
        .snapshot  = post_null_getter,
        .snap_size = 0,
        .serialize = synth_serialize,
        .flags     = BB_CACHE_FLAG_SSE,
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, bb_cache_post("test.post.nosnap"));
}

// bb_cache_post's "data" bb_json_obj_new() failure.
void test_bb_cache_post_data_obj_alloc_failure_returns_no_space(void)
{
    reset_all();
    bb_event_init(NULL);
    bb_cache_config_t cfg = {
        .key = "test.post.dataoom", .snapshot = NULL, .snap_size = sizeof(synth_snap_t),
        .serialize = synth_serialize, .flags = BB_CACHE_FLAG_SSE,
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));
    synth_snap_t s = {.value = 1, .flag = false, .ratio = 0.0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.post.dataoom", .snap = &s }));

    bb_json_host_force_alloc_fail_after(0);  // 1st bb_json_obj_new() call -- "data"
    bb_err_t err = bb_cache_post("test.post.dataoom");
    bb_json_host_force_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
}

// bb_cache_post's "root" bb_json_obj_new() failure (data succeeds, root fails).
void test_bb_cache_post_root_obj_alloc_failure_returns_no_space(void)
{
    reset_all();
    bb_event_init(NULL);
    bb_cache_config_t cfg = {
        .key = "test.post.rootoom", .snapshot = NULL, .snap_size = sizeof(synth_snap_t),
        .serialize = synth_serialize, .flags = BB_CACHE_FLAG_SSE,
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));
    synth_snap_t s = {.value = 1, .flag = false, .ratio = 0.0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.post.rootoom", .snap = &s }));

    bb_json_host_force_alloc_fail_after(1);  // 2nd bb_json_obj_new() call -- "root"
    bb_err_t err = bb_cache_post("test.post.rootoom");
    bb_json_host_force_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
}

// bb_cache_post's final bb_json_serialize(root) failure.
void test_bb_cache_post_serialize_failure_returns_no_space(void)
{
    reset_all();
    bb_event_init(NULL);
    bb_cache_config_t cfg = {
        .key = "test.post.serfail", .snapshot = NULL, .snap_size = sizeof(synth_snap_t),
        .serialize = synth_serialize, .flags = BB_CACHE_FLAG_SSE,
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));
    synth_snap_t s = {.value = 1, .flag = false, .ratio = 0.0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.post.serfail", .snap = &s }));

    bb_json_host_force_serialize_fail_after(0);
    bb_err_t err = bb_cache_post("test.post.serfail");
    bb_json_host_force_serialize_fail_after(-1);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
}

void test_bb_cache_serialize_into_null_args_return_invalid_arg(void)
{
    reset_all();
    reg("test.si.args", NULL, sizeof(synth_snap_t), synth_serialize);
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_serialize_into(NULL, obj));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_serialize_into("test.si.args", NULL));
    bb_json_free(obj);
}

void test_bb_cache_post_serialized_null_args_return_invalid_arg(void)
{
    reset_all();
    reg("test.ps.args", NULL, sizeof(synth_snap_t), synth_serialize);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_post_serialized(NULL, "{}", 2));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_post_serialized("test.ps.args", NULL, 0));
}

void test_bb_cache_post_serialized_unknown_key_returns_not_found(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_cache_post_serialized("no.such.ps.key", "{}", 2));
}

// bb_cache_post_serialized on a key registered WITHOUT SSE returns
// BB_ERR_INVALID_STATE (event_topic read under the lock is NULL).
void test_bb_cache_post_serialized_no_sse_returns_invalid_state(void)
{
    reset_all();
    bb_cache_config_t cfg = {
        .key = "test.ps.nosse", .snapshot = NULL, .snap_size = sizeof(synth_snap_t),
        .serialize = synth_serialize, .flags = BB_CACHE_FLAG_NONE,
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE,
        bb_cache_post_serialized("test.ps.nosse", "{}", 2));
}

void test_bb_cache_get_serialized_null_args_return_invalid_arg(void)
{
    reset_all();
    reg("test.gs.args", NULL, sizeof(synth_snap_t), synth_serialize);
    char buf[64];
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_get_serialized(NULL, buf, sizeof(buf), NULL));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_get_serialized("test.gs.args", NULL, sizeof(buf), NULL));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_cache_get_serialized("test.gs.args", buf, 0, NULL));
}

// bb_cache_get_serialized's fresh-serialize bb_json_obj_new() failure.
void test_bb_cache_get_serialized_obj_alloc_failure_returns_no_space(void)
{
    reset_all();
    reg("test.gs.objoom", NULL, sizeof(synth_snap_t), synth_serialize);
    synth_snap_t s = {.value = 1, .flag = false, .ratio = 0.0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.gs.objoom", .snap = &s }));

    char buf[128];
    bb_json_host_force_alloc_fail_after(0);
    bb_err_t err = bb_cache_get_serialized("test.gs.objoom", buf, sizeof(buf), NULL);
    bb_json_host_force_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
}

// bb_cache_get_serialized's fresh-serialize bb_json_serialize() failure.
void test_bb_cache_get_serialized_serialize_failure_returns_no_space(void)
{
    reset_all();
    reg("test.gs.serfail", NULL, sizeof(synth_snap_t), synth_serialize);
    synth_snap_t s = {.value = 1, .flag = false, .ratio = 0.0};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.gs.serfail", .snap = &s }));

    char buf[128];
    bb_json_host_force_serialize_fail_after(0);
    bb_err_t err = bb_cache_get_serialized("test.gs.serfail", buf, sizeof(buf), NULL);
    bb_json_host_force_serialize_fail_after(-1);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
}

// bb_cache_get_serialized's out_len==NULL on the SUCCESS path (distinct from
// the undersized-buffer failure path, which also passes NULL but returns
// before reaching the out_len write).
void test_bb_cache_get_serialized_null_out_len_on_success(void)
{
    reset_all();
    reg("test.gs.nooutlen", NULL, sizeof(synth_snap_t), synth_serialize);
    synth_snap_t s = {.value = 3, .flag = true, .ratio = 0.5};
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "test.gs.nooutlen", .snap = &s }));

    char buf[128];
    bb_err_t err = bb_cache_get_serialized("test.gs.nooutlen", buf, sizeof(buf), NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"value\":3"));
}

// ---------------------------------------------------------------------------
// B1-592 firmware-review fix: reader-vs-delete TOCTOU/UAF closed by the
// tombstone + generation-guard invariant (see find_entry_locked_ref()'s doc
// comment in bb_cache_espidf.c). Every reader (bb_cache_update/post/
// serialize_into/post_serialized/get_serialized/get_raw) captures an
// (entry, generation) pair under s_reg_lock, then re-validates BOTH key and
// generation immediately after acquiring the entry's own lock -- so a
// concurrent bb_cache_delete() (which never destroys the mutex, only bumps
// generation and frees the slot) can never be observed as a destroyed-mutex
// UB or as another key's bytes under this key's name.
//
// This test hammers bb_cache_get_serialized + bb_cache_get_raw on a single
// churning slot from one thread while a second thread repeatedly deletes and
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

static void mt_churn_serialize(bb_json_t obj, const void *snap)
{
    const mt_churn_snap_t *s = (const mt_churn_snap_t *)snap;
    bb_json_obj_set_int(obj, "value", s->value);
}

typedef struct {
    int unexpected_err_count; // any error other than BB_OK/BB_ERR_NOT_FOUND
    int poisoned_count;       // observed a value that belongs to the OTHER key
} mt_churn_result_t;

// Registers key with SSE so every reader entry point (including
// bb_cache_post/post_serialized) can be exercised against the churning slot.
static bb_err_t mt_churn_register(const char *key)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = NULL,
        .snap_size = sizeof(mt_churn_snap_t),
        .serialize = mt_churn_serialize,
        .flags     = BB_CACHE_FLAG_SSE,
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

// Exercises every reader entry point that captures (entry, generation) via
// find_entry_locked_ref() and re-validates under e->lock: get_serialized,
// get_raw, update, serialize_into, post, post_serialized. Any return other
// than BB_OK/BB_ERR_NOT_FOUND/BB_ERR_INVALID_STATE (the last only possible
// for post/post_serialized before the SSE topic exists) is unexpected;
// observing the OTHER key's value under this key's name is poisoning.
static void mt_churn_check_one(const char *key, int expected, mt_churn_result_t *res)
{
    char buf[128];
    size_t len = 0;
    bb_err_t err = bb_cache_get_serialized(key, buf, sizeof(buf), &len);
    if (err == BB_OK) {
        bb_json_t root = bb_json_parse(buf, len);
        if (root) {
            bb_json_t data = bb_json_obj_get_item(root, "data");
            double v = -1;
            // 0 is a legitimate transient (register succeeded, update
            // hasn't landed under this key's incarnation yet) -- only a
            // non-zero value that belongs to the OTHER key is poisoning.
            if (data && bb_json_obj_get_number(data, "value", &v) &&
                (int)v != 0 && (int)v != expected) {
                res->poisoned_count++;
            }
            bb_json_free(root);
        }
    } else if (err != BB_ERR_NOT_FOUND) {
        res->unexpected_err_count++;
    }

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

    bb_json_t obj = bb_json_obj_new();
    if (obj) {
        bb_err_t serr = bb_cache_serialize_into(key, obj);
        if (serr == BB_OK) {
            double v = -1;
            if (bb_json_obj_get_number(obj, "value", &v) &&
                (int)v != 0 && (int)v != expected) {
                res->poisoned_count++;
            }
        } else if (serr != BB_ERR_NOT_FOUND) {
            res->unexpected_err_count++;
        }
        bb_json_free(obj);
    }
    // bb_cache_post/bb_cache_post_serialized are deliberately NOT exercised
    // in this hot loop: both funnel through bb_event_post's small bounded
    // queue (CONFIG_BB_EVENT_QUEUE_DEPTH, default 16) with no pump in this
    // loop to drain it, so a sustained flood would legitimately return
    // BB_ERR_NO_SPACE (queue full) at a rate that swamps any real signal --
    // that is a queue-capacity artifact of this test's shape, not a bug.
    // serialize_locked's entry_matches_locked check (shared by both
    // bb_cache_post and bb_cache_serialize_into) is already exercised above
    // via serialize_into; bb_cache_post_serialized's own inline check is
    // covered by the low-volume, pump-per-call race test below.
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
    bb_event_init(NULL);

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
// bb_cache_post_serialized's own inline entry_matches_locked re-validation
// (distinct call site from serialize_locked, which bb_cache_post and
// bb_cache_serialize_into share and which the race test above already
// exercises via serialize_into). Deliberately LOW volume with an immediate
// bb_event_pump(0) after every post -- bb_event_post funnels into a small
// bounded queue (CONFIG_BB_EVENT_QUEUE_DEPTH, default 16); pumping after
// every call keeps it drained so this test measures the delete-race
// behavior, not queue capacity.
// ---------------------------------------------------------------------------

#define B1_592_POST_ITERS 60000

typedef struct {
    int unexpected_err_count; // anything other than OK/NOT_FOUND/NO_SPACE
} post_race_result_t;

static void *b1_592_post_deleter_fn(void *arg)
{
    (void)arg;
    static const char *pattern[3] = {
        "test.mt.pchurn", "test.mt.pchurn", "test.mt.pchurnB"
    };
    for (int i = 0; i < B1_592_POST_ITERS; i++) {
        const char *cur = pattern[i % 3];
        bb_cache_delete("test.mt.pchurn");
        bb_cache_delete("test.mt.pchurnB");
        mt_churn_register(cur);
        // A THIRD registry mutation per iteration (matching the "busier
        // deleter" shape of the get_serialized/get_raw race test above,
        // which reliably hits its own mismatch branches) -- widens the
        // window in which a reader's find-then-lock gap spans a full
        // incarnation change.
        mt_churn_snap_t s = { .value = 1 };
        bb_cache_update(&(bb_cache_update_t){ .key = cur, .snap = &s });
    }
    bb_cache_delete("test.mt.pchurn");
    bb_cache_delete("test.mt.pchurnB");
    mt_churn_register("test.mt.pchurn");
    return NULL;
}

static void *b1_592_post_reader_fn(void *arg)
{
    post_race_result_t *res = (post_race_result_t *)arg;
    for (int i = 0; i < B1_592_POST_ITERS; i++) {
        bb_err_t err = bb_cache_post_serialized("test.mt.pchurn", "{\"value\":0}", 11);
        bb_event_pump(0);
        if (err != BB_OK && err != BB_ERR_NOT_FOUND && err != BB_ERR_NO_SPACE) {
            res->unexpected_err_count++;
        }

        err = bb_cache_post_serialized("test.mt.pchurnB", "{\"value\":0}", 11);
        bb_event_pump(0);
        if (err != BB_OK && err != BB_ERR_NOT_FOUND && err != BB_ERR_NO_SPACE) {
            res->unexpected_err_count++;
        }
    }
    return NULL;
}

void test_bb_cache_post_serialized_delete_race_never_crashes(void)
{
    reset_all();
    bb_event_init(NULL);

    post_race_result_t res_a = {0};
    post_race_result_t res_b = {0};

    pthread_t deleter_tid, reader_tid_a, reader_tid_b;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&deleter_tid, NULL, b1_592_post_deleter_fn, NULL));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&reader_tid_a, NULL, b1_592_post_reader_fn, &res_a));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&reader_tid_b, NULL, b1_592_post_reader_fn, &res_b));

    pthread_join(deleter_tid, NULL);
    pthread_join(reader_tid_a, NULL);
    pthread_join(reader_tid_b, NULL);

    TEST_ASSERT_EQUAL_INT(0, res_a.unexpected_err_count);
    TEST_ASSERT_EQUAL_INT(0, res_b.unexpected_err_count);
    TEST_ASSERT_TRUE(bb_cache_exists("test.mt.pchurn"));
}

// ---------------------------------------------------------------------------
// Deterministic tombstone/generation-mismatch coverage (line-coverage
// follow-up to the B1-592 firmware-review fix above). The multi-threaded race
// test above is schedule-dependent -- it reliably hits these mismatch
// branches on some hosts but not others within a bounded iteration count
// (observed: hit on macOS, missed on the Linux CI runner, which is what
// tripped Coveralls' patch-line gate on the four call sites below despite
// 100% branch coverage). bb_cache_test_set_race_hook() (BB_CACHE_TESTING)
// lets these tests deterministically, single-threadedly reproduce the exact
// interleaving the guard exists to close: delete-and-re-register the SAME
// key between find_entry_locked_ref()'s capture and the entry's own lock
// acquisition, guaranteeing a generation mismatch on the very next check.
// ---------------------------------------------------------------------------
void bb_cache_test_set_race_hook(void (*hook)(const char *key));

typedef struct { int value; } race_snap_t;

static void race_serialize(bb_json_t obj, const void *snap)
{
    const race_snap_t *s = (const race_snap_t *)snap;
    bb_json_obj_set_int(obj, "value", s->value);
}

static bb_err_t race_register(const char *key)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = NULL,
        .snap_size = sizeof(race_snap_t),
        .serialize = race_serialize,
        .flags     = BB_CACHE_FLAG_SSE,
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

// Covers serialize_locked's tombstone-mismatch branch (shared by
// bb_cache_post and bb_cache_serialize_into) via the serialize_into caller.
void test_bb_cache_serialize_into_delete_race_returns_not_found(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_OK, race_register("test.race.serialize"));

    bb_cache_test_set_race_hook(race_delete_and_reregister);

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_err_t err = bb_cache_serialize_into("test.race.serialize", obj);
    bb_json_free(obj);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, err);
    // Hook re-registered the key under a new generation -- confirm the slot
    // is alive again post-race, not left tombstoned.
    TEST_ASSERT_TRUE(bb_cache_exists("test.race.serialize"));
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

// Covers bb_cache_get_serialized's own inline tombstone-mismatch branch.
void test_bb_cache_get_serialized_delete_race_returns_not_found(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_OK, race_register("test.race.getser"));

    bb_cache_test_set_race_hook(race_delete_and_reregister);

    char buf[128];
    bb_err_t err = bb_cache_get_serialized("test.race.getser", buf, sizeof(buf), NULL);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, err);
    TEST_ASSERT_TRUE(bb_cache_exists("test.race.getser"));
}

// Covers bb_cache_get_raw's own inline tombstone-mismatch branch.
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

// Covers bb_cache_post_serialized's own inline tombstone-mismatch branch --
// the fifth call site (distinct from serialize_locked, update, get_serialized,
// and get_raw above). The threaded race test
// (test_bb_cache_post_serialized_delete_race_never_crashes) exercises this
// call site too, but only via schedule-dependent thread timing; this
// deterministic hook test is what actually guarantees the mismatch branch
// fires on every host/CI combination.
void test_bb_cache_post_serialized_delete_race_returns_not_found(void)
{
    reset_all();
    bb_event_init(NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, race_register("test.race.postser"));

    bb_cache_test_set_race_hook(race_delete_and_reregister);

    bb_err_t err = bb_cache_post_serialized("test.race.postser", "{\"value\":0}", 11);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, err);
    TEST_ASSERT_TRUE(bb_cache_exists("test.race.postser"));
}

// Covers entry_matches_locked's key-empty (tombstoned, not re-registered)
// short-circuit branch -- distinct from every race test above, which all
// delete-AND-reregister (hitting the generation-mismatch branch instead).
// A bare delete with no re-register leaves key[0] == '\0', so this exercises
// the FIRST condition's false path rather than the second.
void test_bb_cache_serialize_into_delete_only_race_returns_not_found(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_OK, race_register("test.race.delonly"));

    bb_cache_test_set_race_hook(race_delete_only);

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_err_t err = bb_cache_serialize_into("test.race.delonly", obj);
    bb_json_free(obj);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, err);
    // Hook did NOT re-register -- the slot stays tombstoned.
    TEST_ASSERT_FALSE(bb_cache_exists("test.race.delonly"));
}

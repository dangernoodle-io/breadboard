// Host equality harness for bb_cache.
//
// Proves the same-serializer guarantee: event-side serialization (via
// bb_cache_serialize_into on a fresh obj) byte-equals REST-side serialization
// (bb_cache_serialize_into on another fresh obj) for every registered topic.
//
// Synthetic topic registered here; real topics (net.health, diag.boot,
// update.available, health.display, build) wire in below in the TOPIC TABLE
// comment with one entry each.
//
// EXTENSION POINT: To add a real topic, add a row to the `s_topics[]` table
// below following the bb_cache_fidelity_topic_t shape.

#include "unity.h"
#include "bb_cache.h"
#include "bb_diag_event_priv.h"
#include "bb_display_info_event_priv.h"
#include "bb_event.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_net_health.h"
#include "bb_ota_check_internal.h"
#include "../../components/bb_info/src/bb_info_build_priv.h"

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
    .state                  = BB_NET_STATE_GOOD,
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
    .http_connected         = false,
    .http_consec_failures   = 0,
    .http_tls_fail          = 0,
    .http_last_status       = 0,
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

static const bb_info_build_snap_t s_build_initial = {
    .version      = "1.0.0",
    .idf_version  = "v5.3.1",
    .build_date   = "Jan  1 2025",
    .build_time   = "12:00:00",
    .project_name = "breadboard",
    .chip_model   = "ESP32",
    .chip_revision = 3,
    .cores        = 2,
    .cpu_freq_mhz = 240,
    .flash_size   = 4194304,
    .app_size     = 1200000,
    .board        = "wroom32",
    .app_sha256   = "deadbeef0",
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
    {
        .name         = "build",
        .snapshot     = NULL,
        .snap_size    = sizeof(bb_info_build_snap_t),
        .serialize    = bb_info_build_emit,
        .initial_snap = &s_build_initial,
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
        err = bb_cache_update(t->name, t->initial_snap);
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
    bb_cache_update("test.synth", &s1);
    bb_json_t obj1 = bb_json_obj_new();
    bb_cache_serialize_into("test.synth", obj1);
    char *j1 = bb_json_serialize(obj1);

    synth_snap_t s2 = {.value = 99, .flag = true, .ratio = 3.14};
    bb_cache_update("test.synth", &s2);
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
    bb_err_t err = bb_cache_update("no.such.topic", &s);
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
    bb_cache_update("test.enum.a", &s);
    reg("test.enum.b", NULL, sizeof(synth_snap_t), synth_serialize);
    bb_cache_update("test.enum.b", &s);
    reg("test.enum.c", NULL, sizeof(synth_snap_t), synth_serialize);
    bb_cache_update("test.enum.c", &s);

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
        const char *topic = (const char *)0x1; // sentinel to detect no-write
        bb_err_t err = bb_cache_key_at((size_t)i, &topic);
        TEST_ASSERT_EQUAL_INT(BB_OK, err);
        if (topic != NULL) {
            TEST_ASSERT_EQUAL_STRING("test.enum.only", topic);
            found_registered = true;
        }
    }
    TEST_ASSERT_TRUE(found_registered);

    // OOB index
    const char *out = NULL;
    bb_err_t err = bb_cache_key_at((size_t)BB_CACHE_MAX_TOPICS, &out);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, err);
}

// Verify bb_cache_key_at rejects a NULL out pointer.
void test_bb_cache_key_at_null_out_returns_invalid_arg(void)
{
    reset_all();
    bb_err_t err = bb_cache_key_at(0, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
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
    bb_err_t err = bb_cache_update("test.raw", &in);
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
    bb_cache_update("test.raw.args", &s);

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
        bb_cache_update(s_b1_568_names[i], &s);
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
    err = bb_cache_update("test.b1568.stable", &stable);
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
    err = bb_cache_update("test.cfg.basic", &s);
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
    err = bb_cache_update("test.uaf.key", &s);
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
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update("test.cfg.noflags", &s));

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
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_update("test.cfg.sseflag", &s));

    bb_err_t err = bb_cache_post("test.cfg.sseflag");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

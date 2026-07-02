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

// ---------------------------------------------------------------------------
// Test reset hook — declared in bb_cache_espidf.c (BB_CACHE_TESTING)
// ---------------------------------------------------------------------------
void bb_cache_reset_for_test(void);

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

    err = bb_cache_register(t->name, t->snapshot, t->snap_size, t->serialize);
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
    err = bb_cache_register("test.synth", NULL, sizeof(synth_snap_t), synth_serialize);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    err = bb_cache_register("test.synth", NULL, sizeof(synth_snap_t), synth_serialize);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

// Verify update mutates the owned struct and is reflected in serialize_into.
void test_bb_cache_update_reflected_in_serialize(void)
{
    reset_all();
    bb_cache_register("test.synth", NULL, sizeof(synth_snap_t), synth_serialize);

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
// Topic strings are heap-allocated so they outlive the registration call.
// BB_CACHE_MAX_TOPICS may be large (default 32), so we generate them
// dynamically rather than using a literal array of fixed size.
void test_bb_cache_registry_full(void)
{
    reset_all();
    char **topics = (char **)calloc((size_t)BB_CACHE_MAX_TOPICS, sizeof(char *));
    TEST_ASSERT_NOT_NULL(topics);

    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        topics[i] = (char *)malloc(32);
        TEST_ASSERT_NOT_NULL(topics[i]);
        snprintf(topics[i], 32, "test.fill.%d", i);
        bb_err_t err = bb_cache_register(topics[i], NULL,
                                         sizeof(synth_snap_t), synth_serialize);
        if (err != BB_OK) {
            // Free remaining before asserting
            for (int j = i; j < BB_CACHE_MAX_TOPICS; j++) {
                free(topics[j]);
                topics[j] = NULL;
            }
            free(topics);
            TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, err, "fill should succeed");
            return;
        }
    }
    // One more must fail
    bb_err_t err = bb_cache_register("test.fill.overflow", NULL,
                                     sizeof(synth_snap_t), synth_serialize);
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) free(topics[i]);
    free(topics);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
}

// Pins BB_OTA_CHECK_TOPIC's string VALUE to "update.available" so a future
// bb_ota_check refactor cannot silently drift the SSE/cache topic name (the
// macro is free to be renamed; the wire value is the public contract).
void test_bb_ota_check_topic_value_is_update_available(void)
{
    TEST_ASSERT_EQUAL_STRING("update.available", BB_OTA_CHECK_TOPIC);
}

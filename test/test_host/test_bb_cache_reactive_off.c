// Runtime-tests the BB_CACHE_REACTIVE_ENABLE=0 (feature-disabled) path
// (LOW-3, firmware review of B1-589 PR-4b): platformio.ini's [env:native]
// forces -DBB_CACHE_REACTIVE_ENABLE=1 so the static-inline off-path
// (bb_cache_reactive_observe -> BB_ERR_UNSUPPORTED; bb_cache_reactive_update
// -> passthrough to bb_cache_update) was previously only compile-checked by
// the esp32c3 smoke build, never runtime-asserted.
//
// #undef-ing the macro before including the header causes bb_cache_reactive.h's
// own Kconfig-bridge (#ifndef BB_CACHE_REACTIVE_ENABLE / #define ... 0) to
// fall through to its disabled default -- but ONLY in this translation unit.
// Every other TU in the same test binary still includes the header with
// BB_CACHE_REACTIVE_ENABLE=1 (from the command-line -D) and links against
// the real bb_cache_reactive_espidf.c implementation, so this is a targeted
// single-file runtime assertion of the off-path, not a second build config.
#undef BB_CACHE_REACTIVE_ENABLE

#include "unity.h"
#include "bb_cache.h"
#include "bb_cache_reactive.h"

#include <string.h>
#include <stdio.h>

void bb_cache_reset_for_test(void);

typedef struct {
    int value;
} off_snap_t;

static void off_serialize(bb_json_t obj, const void *snap)
{
    const off_snap_t *s = (const off_snap_t *)snap;
    bb_json_obj_set_int(obj, "value", s->value);
}

static void off_setup(void)
{
    bb_cache_reset_for_test();
}

static void off_on_change(const char *key, const char *json, size_t len,
                           int64_t ts_ms, void *ctx)
{
    (void)key; (void)json; (void)len; (void)ts_ms; (void)ctx;
}

// (a) bb_cache_reactive_observe -> BB_ERR_UNSUPPORTED, regardless of cfg
// contents -- the off-path static inline ignores cfg entirely (even NULL),
// unlike the real impl which validates cfg first.
void test_bb_cache_reactive_off_observe_returns_unsupported(void)
{
    off_setup();
    bb_cache_reactive_observer_t cfg = {
        .key = "off.a", .on_change = off_on_change,
    };
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_cache_reactive_observe(&cfg));
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_cache_reactive_observe(NULL));
}

// (b) bb_cache_reactive_update behaves byte-for-byte identically to a direct
// bb_cache_update call: same return, same stored value, same out_changed --
// because the off-path body IS `return bb_cache_update(req);` (pinned
// passthrough, bb_cache_reactive.h).
void test_bb_cache_reactive_off_update_matches_bb_cache_update(void)
{
    off_setup();
    bb_cache_config_t cfg = {
        .key       = "off.a",
        .snapshot  = NULL,
        .snap_size = sizeof(off_snap_t),
        .serialize = off_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_register(&cfg));

    off_snap_t snap = { .value = 1 };
    bool changed = false;
    bb_cache_update_t req = { .key = "off.a", .snap = &snap, .out_changed = &changed };

    // First write: bb_cache_update reports "changed" (first-write semantics).
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req));
    TEST_ASSERT_TRUE(changed);

    char buf[256];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_get_serialized("off.a", buf, sizeof(buf), &out_len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"value\":1"));

    // Identical rewrite: unchanged -> out_changed reflects false, same as a
    // direct bb_cache_update call would report.
    changed = true; // poison to prove it gets overwritten
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req));
    TEST_ASSERT_FALSE(changed);

    // Different value: changed -> BB_OK, out_changed=true, stored value updates.
    snap.value = 2;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_reactive_update(&req));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_get_serialized("off.a", buf, sizeof(buf), &out_len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"value\":2"));

    // Unknown key: error propagates unchanged (no observer machinery exists
    // in the off-path to swallow or alter it).
    bb_cache_update_t req_missing = { .key = "off.missing", .snap = &snap };
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_cache_reactive_update(&req_missing));
}

void test_bb_cache_reactive_off_update_null_req_returns_invalid_arg(void)
{
    off_setup();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_cache_reactive_update(NULL));
}

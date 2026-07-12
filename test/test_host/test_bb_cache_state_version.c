// Tests for bb_cache state_version + bb_cache_snapshot (B1-767 PR-3):
// per-key monotonic write counter + walk-safe copy-under-lock snapshot,
// additive to the existing cached_json/dirty machinery.

#include "unity.h"
#include "bb_cache.h"
#include "bb_json.h"

#include <string.h>
#include <stdint.h>

// Test hooks (BB_CACHE_TESTING): defined in
// platform/espidf/bb_cache/bb_cache_espidf.c.
void bb_cache_reset_for_test(void);
void bb_cache_test_set_clock(uint64_t (*fn)(void));
void bb_cache_test_set_race_hook(void (*hook)(const char *key));

// ---------------------------------------------------------------------------
// Test topic
// ---------------------------------------------------------------------------

typedef struct {
    int value;
} sv_snap_t;

static void sv_serialize(bb_json_t obj, const void *snap)
{
    const sv_snap_t *s = (const sv_snap_t *)snap;
    bb_json_obj_set_int(obj, "value", s->value);
}

static bb_err_t sv_reg_owned(const char *key)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = NULL,
        .snap_size = sizeof(sv_snap_t),
        .serialize = sv_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    return bb_cache_register(&cfg);
}

static sv_snap_t s_getter_backing = { .value = 5 };
static const void *sv_getter(void) { return &s_getter_backing; }

static void sv_reset(void)
{
    bb_cache_reset_for_test();
}

// ---------------------------------------------------------------------------
// bb_cache_state_version -- monotonic, per-key, distinct from generation
// ---------------------------------------------------------------------------

void test_bb_cache_state_version_monotonic_across_writes(void)
{
    sv_reset();
    TEST_ASSERT_EQUAL(BB_OK, sv_reg_owned("sv.a"));

    uint32_t v = 0xFFFFFFFF;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_state_version("sv.a", &v));
    TEST_ASSERT_EQUAL_UINT32(0, v);

    sv_snap_t s = { .value = 1 };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "sv.a", .snap = &s }));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_state_version("sv.a", &v));
    TEST_ASSERT_EQUAL_UINT32(1, v);

    s.value = 2;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "sv.a", .snap = &s }));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_state_version("sv.a", &v));
    TEST_ASSERT_EQUAL_UINT32(2, v);

    s.value = 3;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "sv.a", .snap = &s }));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_state_version("sv.a", &v));
    TEST_ASSERT_EQUAL_UINT32(3, v);
}

void test_bb_cache_state_version_unwritten_registered_key_is_zero(void)
{
    sv_reset();
    TEST_ASSERT_EQUAL(BB_OK, sv_reg_owned("sv.unwritten"));

    uint32_t v = 99;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_state_version("sv.unwritten", &v));
    TEST_ASSERT_EQUAL_UINT32(0, v);
}

// An UNCHANGED rewrite (identical value) still bumps state_version --
// state_version counts WRITES, not CHANGES (contrast with bb_cache_update's
// out_changed, which reports false on an identical rewrite).
void test_bb_cache_state_version_unchanged_rewrite_still_bumps(void)
{
    sv_reset();
    TEST_ASSERT_EQUAL(BB_OK, sv_reg_owned("sv.rewrite"));

    sv_snap_t s = { .value = 7 };
    bool changed = true;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){
        .key = "sv.rewrite", .snap = &s, .out_changed = &changed }));
    TEST_ASSERT_TRUE(changed);  // first write is always "changed"

    uint32_t v1 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_state_version("sv.rewrite", &v1));
    TEST_ASSERT_EQUAL_UINT32(1, v1);

    // Identical rewrite: out_changed reports false, but state_version still
    // bumps -- it counts writes, not changes.
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){
        .key = "sv.rewrite", .snap = &s, .out_changed = &changed }));
    TEST_ASSERT_FALSE(changed);

    uint32_t v2 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_state_version("sv.rewrite", &v2));
    TEST_ASSERT_EQUAL_UINT32(2, v2);
}

void test_bb_cache_state_version_independent_across_keys(void)
{
    sv_reset();
    TEST_ASSERT_EQUAL(BB_OK, sv_reg_owned("sv.x"));
    TEST_ASSERT_EQUAL(BB_OK, sv_reg_owned("sv.y"));

    sv_snap_t s = { .value = 1 };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "sv.x", .snap = &s }));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "sv.x", .snap = &s }));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "sv.y", .snap = &s }));

    uint32_t vx = 0, vy = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_state_version("sv.x", &vx));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_state_version("sv.y", &vy));
    TEST_ASSERT_EQUAL_UINT32(2, vx);
    TEST_ASSERT_EQUAL_UINT32(1, vy);
}

// Distinct from generation: delete + re-register reuses the SLOT (bumping
// generation) but resets state_version to 0 -- the first write after
// re-registration reports version 1, not a continuation of the old count.
void test_bb_cache_state_version_distinct_from_generation_reset_on_reregister(void)
{
    sv_reset();
    TEST_ASSERT_EQUAL(BB_OK, sv_reg_owned("sv.regen"));

    sv_snap_t s = { .value = 1 };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "sv.regen", .snap = &s }));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "sv.regen", .snap = &s }));
    uint32_t v = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_state_version("sv.regen", &v));
    TEST_ASSERT_EQUAL_UINT32(2, v);

    TEST_ASSERT_EQUAL(BB_OK, bb_cache_delete("sv.regen"));
    TEST_ASSERT_EQUAL(BB_OK, sv_reg_owned("sv.regen"));

    TEST_ASSERT_EQUAL(BB_OK, bb_cache_state_version("sv.regen", &v));
    TEST_ASSERT_EQUAL_UINT32(0, v);

    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "sv.regen", .snap = &s }));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_state_version("sv.regen", &v));
    TEST_ASSERT_EQUAL_UINT32(1, v);
}

void test_bb_cache_state_version_getter_mode_key_stays_zero(void)
{
    sv_reset();
    bb_cache_config_t cfg = {
        .key       = "sv.getter",
        .snapshot  = sv_getter,
        .snap_size = 0,
        .serialize = sv_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_register(&cfg));

    uint32_t v = 99;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_state_version("sv.getter", &v));
    TEST_ASSERT_EQUAL_UINT32(0, v);
}

void test_bb_cache_state_version_absent_key_returns_not_found(void)
{
    sv_reset();
    uint32_t v = 42;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_cache_state_version("sv.nope", &v));
    // untouched on absence
    TEST_ASSERT_EQUAL_UINT32(42, v);
}

void test_bb_cache_state_version_null_args_return_invalid_arg(void)
{
    sv_reset();
    TEST_ASSERT_EQUAL(BB_OK, sv_reg_owned("sv.nullargs"));
    uint32_t v = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_cache_state_version(NULL, &v));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_cache_state_version("sv.nullargs", NULL));
}

// ---------------------------------------------------------------------------
// bb_cache_snapshot
// ---------------------------------------------------------------------------

void test_bb_cache_snapshot_returns_current_state_and_version(void)
{
    sv_reset();
    TEST_ASSERT_EQUAL(BB_OK, sv_reg_owned("sv.snap"));

    sv_snap_t s = { .value = 11 };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "sv.snap", .snap = &s }));

    sv_snap_t buf = {0};
    bb_cache_snapshot_t out = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_snapshot("sv.snap", &buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_PTR(&buf, out.state);
    TEST_ASSERT_EQUAL_UINT(sizeof(sv_snap_t), out.size);
    TEST_ASSERT_EQUAL_UINT32(1, out.version);
    TEST_ASSERT_EQUAL_INT(11, buf.value);
}

// Snapshot is an immutable COPY, not a pin: a subsequent mutation of the
// key must not retroactively change the already-returned bytes/version.
void test_bb_cache_snapshot_is_immutable_copy_not_pinned(void)
{
    sv_reset();
    TEST_ASSERT_EQUAL(BB_OK, sv_reg_owned("sv.immutable"));

    sv_snap_t s = { .value = 1 };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "sv.immutable", .snap = &s }));

    sv_snap_t buf = {0};
    bb_cache_snapshot_t out = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_snapshot("sv.immutable", &buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_INT(1, buf.value);
    TEST_ASSERT_EQUAL_UINT32(1, out.version);

    s.value = 999;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "sv.immutable", .snap = &s }));

    // Caller's copy and captured version are unchanged by the later write.
    TEST_ASSERT_EQUAL_INT(1, buf.value);
    TEST_ASSERT_EQUAL_UINT32(1, out.version);
}

void test_bb_cache_snapshot_getter_mode_returns_invalid_state(void)
{
    sv_reset();
    bb_cache_config_t cfg = {
        .key       = "sv.snap.getter",
        .snapshot  = sv_getter,
        .snap_size = 0,
        .serialize = sv_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_register(&cfg));

    sv_snap_t buf = {0};
    bb_cache_snapshot_t out = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_cache_snapshot("sv.snap.getter", &buf, sizeof(buf), &out));
}

void test_bb_cache_snapshot_absent_key_returns_not_found(void)
{
    sv_reset();
    sv_snap_t buf = {0};
    bb_cache_snapshot_t out = {0};
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_cache_snapshot("sv.snap.nope", &buf, sizeof(buf), &out));
}

void test_bb_cache_snapshot_undersized_cap_returns_no_space_buf_untouched(void)
{
    sv_reset();
    TEST_ASSERT_EQUAL(BB_OK, sv_reg_owned("sv.snap.small"));
    sv_snap_t s = { .value = 5 };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "sv.snap.small", .snap = &s }));

    unsigned char tiny[sizeof(sv_snap_t) - 1];
    memset(tiny, 0xAA, sizeof(tiny));
    bb_cache_snapshot_t out = { .state = NULL, .size = 0, .version = 0xDEAD };
    bb_err_t err = bb_cache_snapshot("sv.snap.small", tiny, sizeof(tiny), &out);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_EQUAL_UINT8(0xAA, tiny[0]);
    // out is untouched on refusal too.
    TEST_ASSERT_NULL(out.state);
    TEST_ASSERT_EQUAL_UINT32(0xDEAD, out.version);
}

void test_bb_cache_snapshot_null_args_return_invalid_arg(void)
{
    sv_reset();
    TEST_ASSERT_EQUAL(BB_OK, sv_reg_owned("sv.snap.nullargs"));
    sv_snap_t buf = {0};
    bb_cache_snapshot_t out = {0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_cache_snapshot(NULL, &buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_cache_snapshot("sv.snap.nullargs", NULL, sizeof(buf), &out));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_cache_snapshot("sv.snap.nullargs", &buf, sizeof(buf), NULL));
}

// Owned+fallback: first bb_cache_snapshot() call seeds the cold-start value
// (like bb_cache_get_raw), but the seed is NOT a write -- version stays 0.
static sv_snap_t s_fallback_backing = { .value = 42 };
static const void *sv_fallback_getter(void) { return &s_fallback_backing; }

void test_bb_cache_snapshot_owned_fallback_seeds_on_first_call_version_stays_zero(void)
{
    sv_reset();
    bb_cache_config_t cfg = {
        .key       = "sv.fallback",
        .snapshot  = sv_fallback_getter,
        .snap_size = sizeof(sv_snap_t),
        .serialize = sv_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_register(&cfg));

    sv_snap_t buf = {0};
    bb_cache_snapshot_t out = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_snapshot("sv.fallback", &buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_INT(42, buf.value);   // seeded from the fallback getter
    TEST_ASSERT_EQUAL_UINT32(0, out.version);  // seed is not a write
}

// Plain OWNED (no fallback getter), never written: distinct branch from the
// owned+fallback seed path above -- has_value stays false, e->owned is the
// zero-initialized calloc'd buffer, and no seed ever runs (e->snapshot ==
// NULL short-circuits maybe_seed_fallback()).
void test_bb_cache_snapshot_plain_owned_unwritten_returns_zeroed_state(void)
{
    sv_reset();
    TEST_ASSERT_EQUAL(BB_OK, sv_reg_owned("sv.plain.unwritten"));

    sv_snap_t buf;
    memset(&buf, 0xAA, sizeof(buf));  // poison to prove the memcpy actually ran
    bb_cache_snapshot_t out = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_snapshot("sv.plain.unwritten", &buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_UINT32(0, out.version);
    TEST_ASSERT_EQUAL_UINT(sizeof(sv_snap_t), out.size);
    TEST_ASSERT_EQUAL_INT(0, buf.value);  // zero-initialized owned struct
}

// ---------------------------------------------------------------------------
// Coverage: entry_matches_locked tombstone/generation-mismatch branch in
// both bb_cache_snapshot and bb_cache_state_version -- same deterministic
// race-hook idiom as test_bb_cache_fidelity.c's *_delete_race_returns_not_found
// tests (fires between find_entry_locked_ref()'s capture and the entry's own
// lock acquisition).
// ---------------------------------------------------------------------------

static void sv_race_delete_and_reregister(const char *key)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_delete(key));
    TEST_ASSERT_EQUAL(BB_OK, sv_reg_owned(key));
}

void test_bb_cache_snapshot_delete_race_returns_not_found(void)
{
    sv_reset();
    TEST_ASSERT_EQUAL(BB_OK, sv_reg_owned("sv.race.snap"));

    bb_cache_test_set_race_hook(sv_race_delete_and_reregister);

    sv_snap_t buf = {0};
    bb_cache_snapshot_t out = {0};
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_cache_snapshot("sv.race.snap", &buf, sizeof(buf), &out));
    TEST_ASSERT_TRUE(bb_cache_exists("sv.race.snap"));
}

void test_bb_cache_state_version_delete_race_returns_not_found(void)
{
    sv_reset();
    TEST_ASSERT_EQUAL(BB_OK, sv_reg_owned("sv.race.version"));

    bb_cache_test_set_race_hook(sv_race_delete_and_reregister);

    uint32_t v = 0xDEAD;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_cache_state_version("sv.race.version", &v));
    TEST_ASSERT_EQUAL_UINT32(0xDEAD, v);  // untouched on the race mismatch
    TEST_ASSERT_TRUE(bb_cache_exists("sv.race.version"));
}

// ---------------------------------------------------------------------------
// Coverage: evict_if_aged_out_locked's true branch in bb_cache_snapshot --
// same fake-clock idiom as test_bb_cache_evict.c's *_evicts tests.
// ---------------------------------------------------------------------------

static uint64_t s_sv_fake_now_ms;
static uint64_t sv_fake_clock(void) { return s_sv_fake_now_ms; }

void test_bb_cache_snapshot_evicts_past_evict_age(void)
{
    sv_reset();
    s_sv_fake_now_ms = 0;
    bb_cache_test_set_clock(sv_fake_clock);

    bb_cache_config_t cfg = {
        .key       = "sv.evict",
        .snapshot  = NULL,
        .snap_size = sizeof(sv_snap_t),
        .serialize = sv_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
        .eviction  = {
            .policy       = BB_CACHE_EVICT_AGE_OUT,
            .stale_age_ms = 500,
            .evict_age_ms = 1000,
        },
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_register(&cfg));

    sv_snap_t s = { .value = 1 };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_update(&(bb_cache_update_t){ .key = "sv.evict", .snap = &s }));

    s_sv_fake_now_ms += 1000;

    sv_snap_t buf = {0};
    bb_cache_snapshot_t out = {0};
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_cache_snapshot("sv.evict", &buf, sizeof(buf), &out));

    bb_cache_test_set_clock(NULL);
}

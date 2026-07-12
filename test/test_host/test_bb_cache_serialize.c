// Tests for bb_cache_serialize (B1-767 PR-4): a compositional serialized-
// render cache (render memo) keyed (format_id, key, state_version) that
// reads a bb_cache owned key via bb_cache_snapshot()/bb_cache_state_version()
// (the PR-3 API) and memoizes the rendered wire bytes.

#include "unity.h"
#include "bb_cache.h"
#include "bb_cache_serialize.h"
#include "bb_serialize_json.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Test hooks (BB_CACHE_TESTING): defined in
// platform/espidf/bb_cache/bb_cache_espidf.c.
void bb_cache_reset_for_test(void);

// ---------------------------------------------------------------------------
// Basic fixture: a small owned key, one int64 field.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t value;
} bcs_snap_t;

static const bb_serialize_field_t s_bcs_fields[] = {
    { .key = "value", .type = BB_TYPE_I64, .offset = offsetof(bcs_snap_t, value) },
};

static const bb_serialize_desc_t s_bcs_desc = {
    .type_name = "bcs_snap_t",
    .fields    = s_bcs_fields,
    .n_fields  = 1,
    .snap_size = sizeof(bcs_snap_t),
};

// bb_cache requires a non-NULL serializer fn at register time -- unused by
// this component (it reads via bb_cache_snapshot(), not bb_cache's own
// cached_json/dirty serializer path), so a trivial no-op stands in.
static void bcs_cache_serialize_noop(bb_json_t obj, const void *snap)
{
    (void)obj;
    (void)snap;
}

static bb_err_t bcs_reg_owned(const char *key)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = NULL,
        .snap_size = sizeof(bcs_snap_t),
        .serialize = bcs_cache_serialize_noop,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    return bb_cache_register(&cfg);
}

static bb_err_t bcs_write(const char *key, int64_t value)
{
    bcs_snap_t s = { .value = value };
    return bb_cache_update(&(bb_cache_update_t){ .key = key, .snap = &s });
}

static bcs_snap_t s_bcs_getter_backing = { .value = 5 };
static const void *bcs_getter(void) { return &s_bcs_getter_backing; }

static void bcs_reset(void)
{
    bb_cache_reset_for_test();
    bb_cache_serialize_reset_for_test();
}

// ---------------------------------------------------------------------------
// Overflow fixture: a str_n field whose content exceeds
// BB_CACHE_SERIALIZE_BUF_BYTES (default 512) once rendered as JSON.
// ---------------------------------------------------------------------------

typedef struct {
    bb_serialize_str_n_t s;
} bcs_overflow_snap_t;

static const bb_serialize_field_t s_bcs_overflow_fields[] = {
    { .key = "s", .type = BB_TYPE_STR_N, .offset = offsetof(bcs_overflow_snap_t, s) },
};

static const bb_serialize_desc_t s_bcs_overflow_desc = {
    .type_name = "bcs_overflow_snap_t",
    .fields    = s_bcs_overflow_fields,
    .n_fields  = 1,
    .snap_size = sizeof(bcs_overflow_snap_t),
};

static char s_overflow_str[600];

static bb_err_t bcs_reg_overflow(const char *key)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = NULL,
        .snap_size = sizeof(bcs_overflow_snap_t),
        .serialize = bcs_cache_serialize_noop,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    return bb_cache_register(&cfg);
}

static bb_err_t bcs_write_overflow(const char *key, const char *ptr, size_t len)
{
    bcs_overflow_snap_t s = { .s = { .ptr = ptr, .len = len } };
    return bb_cache_update(&(bb_cache_update_t){ .key = key, .snap = &s });
}

// ---------------------------------------------------------------------------
// 1. HIT -- register+update; get twice at the same version -> identical
//    bytes AND exactly one render.
// ---------------------------------------------------------------------------

void test_bb_cache_serialize_get_hit_same_version_returns_identical_bytes_and_render_count_once(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.hit"));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.hit", 1));

    const uint8_t *out1 = NULL;
    size_t len1 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.hit", &s_bcs_desc, &out1, &len1));
    TEST_ASSERT_EQUAL_UINT(1, bb_cache_serialize_render_count());

    const uint8_t *out2 = NULL;
    size_t len2 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.hit", &s_bcs_desc, &out2, &len2));
    TEST_ASSERT_EQUAL_UINT(1, bb_cache_serialize_render_count());  // no re-render on a hit

    TEST_ASSERT_EQUAL_PTR(out1, out2);   // same borrowed slot
    TEST_ASSERT_EQUAL_UINT(len1, len2);
    TEST_ASSERT_EQUAL_MEMORY(out1, out2, len1);
    TEST_ASSERT_EQUAL_STRING("{\"value\":1}", (const char *)out1);
}

// ---------------------------------------------------------------------------
// 2. MISS/recache -- a bb_cache_update() bump invalidates the memo.
// ---------------------------------------------------------------------------

void test_bb_cache_serialize_get_recache_on_state_version_bump_bumps_render_count(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.recache"));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.recache", 1));

    const uint8_t *out1 = NULL;
    size_t len1 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.recache", &s_bcs_desc, &out1, &len1));
    TEST_ASSERT_EQUAL_UINT(1, bb_cache_serialize_render_count());
    TEST_ASSERT_EQUAL_STRING("{\"value\":1}", (const char *)out1);

    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.recache", 2));

    const uint8_t *out2 = NULL;
    size_t len2 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.recache", &s_bcs_desc, &out2, &len2));
    TEST_ASSERT_EQUAL_UINT(2, bb_cache_serialize_render_count());
    TEST_ASSERT_EQUAL_STRING("{\"value\":2}", (const char *)out2);
}

// ---------------------------------------------------------------------------
// 3. Byte-identity -- memo bytes memcmp-equal to a direct render of the same
//    snapshot.
// ---------------------------------------------------------------------------

void test_bb_cache_serialize_get_byte_identical_to_direct_json_render(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.identity"));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.identity", 42));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.identity", &s_bcs_desc, &out, &out_len));

    bcs_snap_t snap = { .value = 42 };
    char buf[64];
    size_t direct_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_render(&s_bcs_desc, &snap, buf, sizeof(buf), &direct_len));

    TEST_ASSERT_EQUAL_UINT(direct_len, out_len);
    TEST_ASSERT_EQUAL_MEMORY(buf, out, direct_len);
}

// ---------------------------------------------------------------------------
// 4. Unsupported fmt -- BB_FORMAT_JSON ok; BB_FORMAT_NONE unsupported.
// ---------------------------------------------------------------------------

void test_bb_cache_serialize_get_unsupported_format_returns_unsupported(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.unsupported"));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.unsupported", 1));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED,
                       bb_cache_serialize_get(BB_FORMAT_NONE, "bcs.unsupported", &s_bcs_desc, &out, &out_len));
}

// Same key, two different formats: a JSON memo already exists for the key
// (a valid slot) when an unsupported-format get is issued -- exercises the
// slot-exists-but-wrong-format branch inside the (fmt,key) slot lookup,
// distinct from the fresh-key case above (no slot exists at all yet).
void test_bb_cache_serialize_get_unsupported_format_with_existing_json_slot_for_same_key(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.unsupported.existing"));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.unsupported.existing", 1));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.unsupported.existing",
                                                     &s_bcs_desc, &out, &out_len));

    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED,
                       bb_cache_serialize_get(BB_FORMAT_NONE, "bcs.unsupported.existing",
                                               &s_bcs_desc, &out, &out_len));
}

// Unsupported fmt is a STATIC property, checked before the key-existence
// lookup -- a key that was NEVER registered still returns UNSUPPORTED, not
// NOT_FOUND. Distinct from the two tests above (which use a registered key)
// -- this exercises the moved check's independence from bb_cache state.
void test_bb_cache_serialize_get_unsupported_format_absent_key_returns_unsupported(void)
{
    bcs_reset();

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED,
                       bb_cache_serialize_get(BB_FORMAT_NONE, "bcs.unsupported.absent",
                                               &s_bcs_desc, &out, &out_len));
}

// ---------------------------------------------------------------------------
// 5. Key absent -> NOT_FOUND.
// ---------------------------------------------------------------------------

void test_bb_cache_serialize_get_absent_key_returns_not_found(void)
{
    bcs_reset();

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.nope", &s_bcs_desc, &out, &out_len));
}

// ---------------------------------------------------------------------------
// 6. Getter-mode key -> INVALID_STATE (no owned struct to snapshot).
// ---------------------------------------------------------------------------

void test_bb_cache_serialize_get_getter_mode_key_returns_invalid_state(void)
{
    bcs_reset();
    bb_cache_config_t cfg = {
        .key       = "bcs.getter",
        .snapshot  = bcs_getter,
        .snap_size = 0,
        .serialize = bcs_cache_serialize_noop,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_register(&cfg));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.getter", &s_bcs_desc, &out, &out_len));
}

// ---------------------------------------------------------------------------
// 7. Render overflow -> NO_SPACE, slot left invalid; a later fitting get
//    still works.
// ---------------------------------------------------------------------------

void test_bb_cache_serialize_get_render_overflow_returns_no_space_slot_left_invalid(void)
{
    bcs_reset();
    memset(s_overflow_str, 'a', sizeof(s_overflow_str));

    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_overflow("bcs.overflow"));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write_overflow("bcs.overflow", s_overflow_str, sizeof(s_overflow_str)));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.overflow", &s_bcs_overflow_desc, &out, &out_len));

    // A later fitting write+get on the SAME key still works -- the failed
    // render did not poison the slot.
    TEST_ASSERT_EQUAL(BB_OK, bcs_write_overflow("bcs.overflow", "x", 1));
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.overflow", &s_bcs_overflow_desc, &out, &out_len));
    TEST_ASSERT_EQUAL_STRING("{\"s\":\"x\"}", (const char *)out);
}

// ---------------------------------------------------------------------------
// 8. Slot exhaustion -- register MAX_ENTRIES+1 distinct keys, get each; the
//    last get is NO_SPACE, but a previously-cached key still HITs (no live
//    eviction).
// ---------------------------------------------------------------------------

// BB_CACHE_SERIALIZE_MAX_ENTRIES is a private Kconfig-bridge constant (not
// part of the public header) -- this mirrors its C default (8), which
// [env:native] does not override.
#define BCS_TEST_MAX_ENTRIES 8

void test_bb_cache_serialize_get_slot_table_exhaustion_returns_no_space_last_key_previous_still_hits(void)
{
    bcs_reset();

    char keys[BCS_TEST_MAX_ENTRIES + 1][32];
    for (int i = 0; i < BCS_TEST_MAX_ENTRIES + 1; i++) {
        snprintf(keys[i], sizeof(keys[i]), "bcs.slot.%d", i);
        TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned(keys[i]));
        TEST_ASSERT_EQUAL(BB_OK, bcs_write(keys[i], i));
    }

    const uint8_t *out = NULL;
    size_t out_len = 0;
    for (int i = 0; i < BCS_TEST_MAX_ENTRIES; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, keys[i], &s_bcs_desc, &out, &out_len));
    }

    // The (MAX_ENTRIES+1)th distinct (fmt,key) pair has no free slot.
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                       bb_cache_serialize_get(BB_FORMAT_JSON, keys[BCS_TEST_MAX_ENTRIES],
                                               &s_bcs_desc, &out, &out_len));

    // The first key, already memoized, still hits.
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, keys[0], &s_bcs_desc, &out, &out_len));
    TEST_ASSERT_EQUAL_STRING("{\"value\":0}", (const char *)out);
}

// ---------------------------------------------------------------------------
// 9. INVALID_ARG -- null key/desc/out/out_len.
// ---------------------------------------------------------------------------

void test_bb_cache_serialize_get_null_args_return_invalid_arg(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.nullargs"));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.nullargs", 1));

    const uint8_t *out = NULL;
    size_t out_len = 0;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_cache_serialize_get(BB_FORMAT_JSON, NULL, &s_bcs_desc, &out, &out_len));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.nullargs", NULL, &out, &out_len));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.nullargs", &s_bcs_desc, NULL, &out_len));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.nullargs", &s_bcs_desc, &out, NULL));
}

// ---------------------------------------------------------------------------
// 10. Distinct-slot independence -- a MISS on key B after a HIT on key A
//     leaves A's bytes intact.
// ---------------------------------------------------------------------------

void test_bb_cache_serialize_get_distinct_keys_independent_slots(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.indep.a"));
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.indep.b"));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.indep.a", 111));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.indep.b", 222));

    const uint8_t *out_a = NULL;
    size_t len_a = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.indep.a", &s_bcs_desc, &out_a, &len_a));
    char copy_a[64];
    memcpy(copy_a, out_a, len_a);
    copy_a[len_a] = '\0';

    const uint8_t *out_b = NULL;
    size_t len_b = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.indep.b", &s_bcs_desc, &out_b, &len_b));

    const uint8_t *out_a2 = NULL;
    size_t len_a2 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.indep.a", &s_bcs_desc, &out_a2, &len_a2));
    TEST_ASSERT_EQUAL_UINT(len_a, len_a2);
    TEST_ASSERT_EQUAL_MEMORY(copy_a, out_a2, len_a);
    TEST_ASSERT_EQUAL_STRING("{\"value\":111}", (const char *)out_a2);
}

// ---------------------------------------------------------------------------
// Snapshot-oversize fixture: an owned struct whose snap_size exceeds
// BB_CACHE_SERIALIZE_BUF_BYTES (default 512) -- bb_cache_snapshot()'s `tmp`
// scratch (sized BB_CACHE_SERIALIZE_BUF_BYTES) can't hold it, so the
// snapshot itself fails NO_SPACE before rendering is ever attempted. Value-
// tests the snapshot-side NO_SPACE contract distinct from fixture 7 above
// (which is a render-side overflow of a struct that snapshots fine).
// ---------------------------------------------------------------------------

typedef struct {
    int64_t value;
    char    pad[600];   // pushes sizeof(...) past the 512-byte default cap
} bcs_snap_oversize_snap_t;

static const bb_serialize_field_t s_bcs_snap_oversize_fields[] = {
    { .key = "value", .type = BB_TYPE_I64, .offset = offsetof(bcs_snap_oversize_snap_t, value) },
};

static const bb_serialize_desc_t s_bcs_snap_oversize_desc = {
    .type_name = "bcs_snap_oversize_snap_t",
    .fields    = s_bcs_snap_oversize_fields,
    .n_fields  = 1,
    .snap_size = sizeof(bcs_snap_oversize_snap_t),
};

static bb_err_t bcs_reg_snap_oversize(const char *key)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = NULL,
        .snap_size = sizeof(bcs_snap_oversize_snap_t),
        .serialize = bcs_cache_serialize_noop,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    return bb_cache_register(&cfg);
}

static bb_err_t bcs_write_snap_oversize(const char *key, int64_t value)
{
    bcs_snap_oversize_snap_t s = { .value = value };
    return bb_cache_update(&(bb_cache_update_t){ .key = key, .snap = &s });
}

void test_bb_cache_serialize_get_snapshot_oversize_returns_no_space_slot_left_invalid(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_snap_oversize("bcs.snap.oversize"));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write_snap_oversize("bcs.snap.oversize", 1));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.snap.oversize",
                                               &s_bcs_snap_oversize_desc, &out, &out_len));
    TEST_ASSERT_EQUAL_UINT(0, bb_cache_serialize_render_count());  // never reached the render backend

    // The oversize key's failed snapshot leaves its own slot invalid but
    // does not disturb a DIFFERENT, fitting key's slot.
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.snap.fits"));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.snap.fits", 7));
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.snap.fits", &s_bcs_desc, &out, &out_len));
    TEST_ASSERT_EQUAL_STRING("{\"value\":7}", (const char *)out);
}

// ---------------------------------------------------------------------------
// 11. reset_for_test clears the table -- fresh MISS after reset.
// ---------------------------------------------------------------------------

void test_bb_cache_serialize_reset_for_test_clears_table_fresh_miss_after_reset(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.reset"));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.reset", 1));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.reset", &s_bcs_desc, &out, &out_len));
    TEST_ASSERT_EQUAL_UINT(1, bb_cache_serialize_render_count());

    bb_cache_serialize_reset_for_test();
    TEST_ASSERT_EQUAL_UINT(0, bb_cache_serialize_render_count());

    // Same (fmt,key) pair at the SAME state_version now misses again (the
    // memo table was cleared, not just the counter).
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.reset", &s_bcs_desc, &out, &out_len));
    TEST_ASSERT_EQUAL_UINT(1, bb_cache_serialize_render_count());
}

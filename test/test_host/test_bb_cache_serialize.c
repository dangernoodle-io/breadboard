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
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.hit", &s_bcs_desc, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.hit", 1));

    const uint8_t *out1 = NULL;
    size_t len1 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.hit", &out1, &len1));
    TEST_ASSERT_EQUAL_UINT(1, bb_cache_serialize_render_count());

    const uint8_t *out2 = NULL;
    size_t len2 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.hit", &out2, &len2));
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
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.recache", &s_bcs_desc, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.recache", 1));

    const uint8_t *out1 = NULL;
    size_t len1 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.recache", &out1, &len1));
    TEST_ASSERT_EQUAL_UINT(1, bb_cache_serialize_render_count());
    TEST_ASSERT_EQUAL_STRING("{\"value\":1}", (const char *)out1);

    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.recache", 2));

    const uint8_t *out2 = NULL;
    size_t len2 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.recache", &out2, &len2));
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
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.identity", &s_bcs_desc, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.identity", 42));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.identity", &out, &out_len));

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
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.unsupported", &s_bcs_desc, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.unsupported", 1));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED,
                       bb_cache_serialize_get(BB_FORMAT_NONE, "bcs.unsupported", &out, &out_len));
}

// Same key, two different formats: a JSON memo already exists for the key
// (a valid slot) when an unsupported-format get is issued -- exercises the
// slot-exists-but-wrong-format branch inside the (fmt,key) slot lookup,
// distinct from the fresh-key case above (no slot exists at all yet).
void test_bb_cache_serialize_get_unsupported_format_with_existing_json_slot_for_same_key(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.unsupported.existing"));
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_cache_serialize_register("bcs.unsupported.existing", &s_bcs_desc, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.unsupported.existing", 1));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.unsupported.existing",
                                                     &out, &out_len));

    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED,
                       bb_cache_serialize_get(BB_FORMAT_NONE, "bcs.unsupported.existing",
                                               &out, &out_len));
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
                                               &out, &out_len));
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
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.nope", &out, &out_len));
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
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.getter", &s_bcs_desc, NULL, NULL));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.getter", &out, &out_len));
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
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_cache_serialize_register("bcs.overflow", &s_bcs_overflow_desc, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write_overflow("bcs.overflow", s_overflow_str, sizeof(s_overflow_str)));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.overflow", &out, &out_len));

    // A later fitting write+get on the SAME key still works -- the failed
    // render did not poison the slot.
    TEST_ASSERT_EQUAL(BB_OK, bcs_write_overflow("bcs.overflow", "x", 1));
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.overflow", &out, &out_len));
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
        TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register(keys[i], &s_bcs_desc, NULL, NULL));
        TEST_ASSERT_EQUAL(BB_OK, bcs_write(keys[i], i));
    }

    const uint8_t *out = NULL;
    size_t out_len = 0;
    for (int i = 0; i < BCS_TEST_MAX_ENTRIES; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, keys[i], &out, &out_len));
    }

    // The (MAX_ENTRIES+1)th distinct (fmt,key) pair has no free slot.
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                       bb_cache_serialize_get(BB_FORMAT_JSON, keys[BCS_TEST_MAX_ENTRIES],
                                               &out, &out_len));

    // The first key, already memoized, still hits.
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, keys[0], &out, &out_len));
    TEST_ASSERT_EQUAL_STRING("{\"value\":0}", (const char *)out);
}

// ---------------------------------------------------------------------------
// 9. INVALID_ARG -- null key/out/out_len.
// ---------------------------------------------------------------------------

void test_bb_cache_serialize_get_null_args_return_invalid_arg(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.nullargs"));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.nullargs", 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.nullargs", &s_bcs_desc, NULL, NULL));

    const uint8_t *out = NULL;
    size_t out_len = 0;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_cache_serialize_get(BB_FORMAT_JSON, NULL, &out, &out_len));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.nullargs", NULL, &out_len));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.nullargs", &out, NULL));
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
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.indep.a", &s_bcs_desc, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.indep.b", &s_bcs_desc, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.indep.a", 111));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.indep.b", 222));

    const uint8_t *out_a = NULL;
    size_t len_a = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.indep.a", &out_a, &len_a));
    char copy_a[64];
    memcpy(copy_a, out_a, len_a);
    copy_a[len_a] = '\0';

    const uint8_t *out_b = NULL;
    size_t len_b = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.indep.b", &out_b, &len_b));

    const uint8_t *out_a2 = NULL;
    size_t len_a2 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.indep.a", &out_a2, &len_a2));
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
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_cache_serialize_register("bcs.snap.oversize", &s_bcs_snap_oversize_desc,
                                                    NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write_snap_oversize("bcs.snap.oversize", 1));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.snap.oversize", &out, &out_len));
    TEST_ASSERT_EQUAL_UINT(0, bb_cache_serialize_render_count());  // never reached the render backend

    // The oversize key's failed snapshot leaves its own slot invalid but
    // does not disturb a DIFFERENT, fitting key's slot.
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.snap.fits"));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.snap.fits", &s_bcs_desc, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.snap.fits", 7));
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.snap.fits", &out, &out_len));
    TEST_ASSERT_EQUAL_STRING("{\"value\":7}", (const char *)out);
}

// ---------------------------------------------------------------------------
// 11. reset_for_test clears the table -- fresh MISS after reset.
// ---------------------------------------------------------------------------

void test_bb_cache_serialize_reset_for_test_clears_table_fresh_miss_after_reset(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.reset"));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.reset", &s_bcs_desc, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.reset", 1));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.reset", &out, &out_len));
    TEST_ASSERT_EQUAL_UINT(1, bb_cache_serialize_render_count());

    bb_cache_serialize_reset_for_test();
    TEST_ASSERT_EQUAL_UINT(0, bb_cache_serialize_render_count());

    // reset_for_test also clears the descriptor registry -- the key's
    // binding is gone, so a get for the SAME (fmt,key) pair now returns
    // NOT_FOUND rather than re-rendering.
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.reset", &out, &out_len));
    TEST_ASSERT_EQUAL_UINT(0, bb_cache_serialize_render_count());
}

// ---------------------------------------------------------------------------
// 12. reset_for_test clears the descriptor registry AND the slot table --
//     re-registering after a reset re-establishes a fresh binding and a
//     fresh (miss) render, independent of the pre-reset memo.
// ---------------------------------------------------------------------------

void test_bb_cache_serialize_reset_for_test_clears_registry_rebind_after_reset_renders_fresh(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.reset.rebind"));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.reset.rebind", &s_bcs_desc, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.reset.rebind", 1));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.reset.rebind", &out, &out_len));
    TEST_ASSERT_EQUAL_UINT(1, bb_cache_serialize_render_count());

    bb_cache_serialize_reset_for_test();
    bb_cache_reset_for_test();

    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.reset.rebind"));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.reset.rebind", &s_bcs_desc, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.reset.rebind", 9));

    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.reset.rebind", &out, &out_len));
    TEST_ASSERT_EQUAL_UINT(1, bb_cache_serialize_render_count());
    TEST_ASSERT_EQUAL_STRING("{\"value\":9}", (const char *)out);
}

// ---------------------------------------------------------------------------
// PR-5: consumer-registered descriptors + refresh (B1-767).
// ---------------------------------------------------------------------------

// Gather fixture: ctx is an int64_t* holding the value to stamp in.
static bb_err_t bcs_gather_ok(void *dst, void *ctx)
{
    ((bcs_snap_t *)dst)->value = *(int64_t *)ctx;
    return BB_OK;
}

static bb_err_t bcs_gather_fail(void *dst, void *ctx)
{
    (void)dst;
    (void)ctx;
    return BB_ERR_INVALID_STATE;
}

// 13. get on a bb_cache-registered key with NO serialize binding -> NOT_FOUND
// (distinct from the absent-key case: this key IS registered/written in
// bb_cache, only the bb_cache_serialize binding is missing).
void test_bb_cache_serialize_get_bb_cache_registered_key_with_no_serialize_binding_returns_not_found(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.unbound"));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.unbound", 1));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.unbound", &out, &out_len));
}

// 14. register override -- a re-register with a DIFFERENT descriptor
// invalidates the existing memo, and the next get renders with the NEW
// descriptor's fields, not the old one's.
void test_bb_cache_serialize_register_override_invalidates_memo_renders_new_descriptor(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_overflow("bcs.override"));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.override", &s_bcs_overflow_desc, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write_overflow("bcs.override", "x", 1));

    // A DIFFERENT key, memoized before the override -- exercises the
    // valid-slot/key-mismatch branch inside the override's slot invalidation
    // sweep (distinct from the valid-slot/key-match branch it exists to
    // hit), and proves that sweep leaves an unrelated key's memo intact.
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.override.other"));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.override.other", &s_bcs_desc, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.override.other", 77));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.override", &out, &out_len));
    TEST_ASSERT_EQUAL_STRING("{\"s\":\"x\"}", (const char *)out);
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.override.other", &out, &out_len));
    TEST_ASSERT_EQUAL_STRING("{\"value\":77}", (const char *)out);
    TEST_ASSERT_EQUAL_UINT(2, bb_cache_serialize_render_count());

    // Re-register "bcs.override" with a completely different bb_cache
    // registration + descriptor. The memo from the old descriptor must not
    // leak through, and "bcs.override.other"'s memo must be untouched.
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.override"));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.override", &s_bcs_desc, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_OK, bcs_write("bcs.override", 5));

    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.override", &out, &out_len));
    TEST_ASSERT_EQUAL_UINT(3, bb_cache_serialize_render_count());  // re-rendered, not served from memo
    TEST_ASSERT_EQUAL_STRING("{\"value\":5}", (const char *)out);

    TEST_ASSERT_EQUAL(BB_OK,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.override.other", &out, &out_len));
    TEST_ASSERT_EQUAL_UINT(3, bb_cache_serialize_render_count());  // still memoized, untouched
    TEST_ASSERT_EQUAL_STRING("{\"value\":77}", (const char *)out);
}

// 15. register INVALID_ARG -- null key / null desc.
void test_bb_cache_serialize_register_null_args_return_invalid_arg(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_cache_serialize_register(NULL, &s_bcs_desc, NULL, NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_cache_serialize_register("bcs.reg.nullargs", NULL, NULL, NULL));
}

// 16. register NO_SPACE -- fill the registry with distinct keys, the last
// one has no free slot. BCS_TEST_MAX_KEYS is derived from the COMPILED
// BB_CACHE_SERIALIZE_MAX_KEYS macro (not hardcoded) -- [env:native] bumps it
// to 16 via -DBB_CACHE_SERIALIZE_MAX_KEYS=16 (see platformio.ini) so the
// pre-existing slot-exhaustion fixture above (9 distinct keys) can register a
// binding for every key; deriving from the macro keeps this test correct if
// that flag or the Kconfig default ever changes.
#define BCS_TEST_MAX_KEYS BB_CACHE_SERIALIZE_MAX_KEYS

void test_bb_cache_serialize_register_registry_exhaustion_returns_no_space(void)
{
    bcs_reset();

    char keys[BCS_TEST_MAX_KEYS + 1][32];
    for (int i = 0; i < BCS_TEST_MAX_KEYS; i++) {
        snprintf(keys[i], sizeof(keys[i]), "bcs.reg.%d", i);
        TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register(keys[i], &s_bcs_desc, NULL, NULL));
    }

    snprintf(keys[BCS_TEST_MAX_KEYS], sizeof(keys[BCS_TEST_MAX_KEYS]), "bcs.reg.%d", BCS_TEST_MAX_KEYS);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                       bb_cache_serialize_register(keys[BCS_TEST_MAX_KEYS], &s_bcs_desc, NULL, NULL));

    // Re-registering an ALREADY-bound key (override, not a new claim) still
    // succeeds even with the registry full.
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register(keys[0], &s_bcs_desc, NULL, NULL));
}

// 17. refresh -- gather writes value=N, refresh bumps state_version, the
// next get re-renders {"value":N}.
void test_bb_cache_serialize_refresh_gather_bumps_state_version_get_rerenders(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.refresh"));
    int64_t gather_value = 3;
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_cache_serialize_register("bcs.refresh", &s_bcs_desc, bcs_gather_ok, &gather_value));

    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_refresh("bcs.refresh"));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.refresh", &out, &out_len));
    TEST_ASSERT_EQUAL_STRING("{\"value\":3}", (const char *)out);

    gather_value = 42;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_refresh("bcs.refresh"));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.refresh", &out, &out_len));
    TEST_ASSERT_EQUAL_STRING("{\"value\":42}", (const char *)out);
}

// 18. refresh with NULL gather -> INVALID_STATE.
void test_bb_cache_serialize_refresh_null_gather_returns_invalid_state(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.refresh.nogather"));
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_cache_serialize_register("bcs.refresh.nogather", &s_bcs_desc, NULL, NULL));

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_cache_serialize_refresh("bcs.refresh.nogather"));
}

// 19. refresh on an unbound key -> NOT_FOUND.
void test_bb_cache_serialize_refresh_unbound_key_returns_not_found(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_cache_serialize_refresh("bcs.refresh.unbound"));
}

// refresh(NULL) -> INVALID_ARG.
void test_bb_cache_serialize_refresh_null_key_returns_invalid_arg(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_cache_serialize_refresh(NULL));
}

// get() on a key that HAS a bb_cache_serialize binding but was never
// registered in bb_cache itself -- distinct from the "bb_cache-registered,
// no serialize binding" case above: this exercises the bb_cache_state_version
// NOT_FOUND propagation branch reached AFTER the registry lookup succeeds.
void test_bb_cache_serialize_get_serialize_bound_key_not_in_bb_cache_returns_not_found(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_cache_serialize_register("bcs.notincache", &s_bcs_desc, NULL, NULL));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                       bb_cache_serialize_get(BB_FORMAT_JSON, "bcs.notincache", &out, &out_len));
}

// 20. refresh whose bound descriptor's snap_size exceeds the scratch cap ->
// NO_SPACE, gather never invoked.
void test_bb_cache_serialize_refresh_oversize_snap_returns_no_space(void)
{
    bcs_reset();
    int64_t gather_value = 1;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.refresh.oversize", &s_bcs_snap_oversize_desc,
                                                          bcs_gather_ok, &gather_value));

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_cache_serialize_refresh("bcs.refresh.oversize"));
}

// 21. refresh propagates a gather failure without calling bb_cache_update.
void test_bb_cache_serialize_refresh_gather_error_propagates(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_owned("bcs.refresh.gatherfail"));
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_cache_serialize_register("bcs.refresh.gatherfail", &s_bcs_desc, bcs_gather_fail,
                                                    NULL));

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_cache_serialize_refresh("bcs.refresh.gatherfail"));

    // state_version untouched -- bb_cache_update() was never reached.
    uint32_t version = 0xFFFFFFFF;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_state_version("bcs.refresh.gatherfail", &version));
    TEST_ASSERT_EQUAL_UINT(0, version);
}

// 22. refresh propagates the underlying bb_cache_update() error -- the
// binding exists in bb_cache_serialize's registry but the key was never
// registered in bb_cache itself.
void test_bb_cache_serialize_refresh_bb_cache_update_error_propagates(void)
{
    bcs_reset();
    int64_t gather_value = 1;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.refresh.notincache", &s_bcs_desc,
                                                          bcs_gather_ok, &gather_value));

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_cache_serialize_refresh("bcs.refresh.notincache"));
}

// ---------------------------------------------------------------------------
// PR-5 review fix (MED): refresh's read-side bound. reg->desc->snap_size
// only bounds the gather WRITE into tmp -- bb_cache_update() then copies the
// bb_cache-REGISTERED size for the key, which can differ from (or exceed)
// desc->snap_size. refresh() now snapshots first (bb_cache_snapshot), which
// bounds that registered size against sizeof(tmp) too and pre-fills tmp with
// the current value.
// ---------------------------------------------------------------------------

// 23. refresh where the bb_cache-registered struct size exceeds the scratch
// cap, even though the BOUND DESCRIPTOR's snap_size fits -- the snapshot-
// first bound (not the gather-write bound) is what catches this. Gather is
// never invoked (no over-read of tmp is possible).
void test_bb_cache_serialize_refresh_bb_cache_size_exceeds_scratch_cap_returns_no_space(void)
{
    bcs_reset();
    // Register an OVERSIZE owned struct in bb_cache (snap_size > 512), but
    // bind a small, on-contract descriptor (snap_size 8, well under the
    // scratch cap) -- the mismatch this fix closes.
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_snap_oversize("bcs.refresh.bigreg"));
    int64_t gather_value = 1;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.refresh.bigreg", &s_bcs_desc,
                                                          bcs_gather_ok, &gather_value));

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_cache_serialize_refresh("bcs.refresh.bigreg"));

    // state_version untouched -- neither gather nor bb_cache_update was
    // reached.
    uint32_t version = 0xFFFFFFFF;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_state_version("bcs.refresh.bigreg", &version));
    TEST_ASSERT_EQUAL_UINT(0, version);
}

// Wide fixture: bb_cache-registered size (16 bytes) exceeds the bound
// descriptor's snap_size (8 bytes, "value" only) -- gather only ever
// touches the first 8 bytes of tmp.
typedef struct {
    int64_t value;
    int64_t tail;
} bcs_wide_snap_t;

static bb_err_t bcs_reg_wide(const char *key)
{
    bb_cache_config_t cfg = {
        .key       = key,
        .snapshot  = NULL,
        .snap_size = sizeof(bcs_wide_snap_t),
        .serialize = bcs_cache_serialize_noop,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    return bb_cache_register(&cfg);
}

// 24. refresh where the bound descriptor's snap_size (8) is smaller than the
// bb_cache-registered size (16) -- the pre-filled snapshot means the tail
// bytes gather never touches are stored as the CURRENT value, not
// uninitialized stack garbage.
void test_bb_cache_serialize_refresh_partial_gather_preserves_untouched_tail(void)
{
    bcs_reset();
    TEST_ASSERT_EQUAL(BB_OK, bcs_reg_wide("bcs.refresh.wide"));
    bcs_wide_snap_t seed = { .value = 0, .tail = 999 };
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_cache_update(&(bb_cache_update_t){ .key = "bcs.refresh.wide", .snap = &seed }));

    // s_bcs_desc only describes "value" (snap_size == sizeof(bcs_snap_t),
    // 8 bytes) -- gather (bcs_gather_ok) only ever writes those 8 bytes.
    int64_t gather_value = 55;
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_register("bcs.refresh.wide", &s_bcs_desc, bcs_gather_ok,
                                                          &gather_value));
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_refresh("bcs.refresh.wide"));

    bcs_wide_snap_t readback = {0};
    bb_cache_snapshot_t snap = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_snapshot("bcs.refresh.wide", &readback, sizeof(readback), &snap));
    TEST_ASSERT_EQUAL_INT64(55, readback.value);     // gather-written
    TEST_ASSERT_EQUAL_INT64(999, readback.tail);     // preserved, not garbage
}

// ---------------------------------------------------------------------------
// PR-5 review fix (LOW): register rejects over-length keys instead of
// silently truncating (mirrors bb_cache_register()'s own contract).
// ---------------------------------------------------------------------------

// 25. register with a key >= BB_CACHE_KEY_MAX -> INVALID_ARG, no slot
// claimed/overridden.
void test_bb_cache_serialize_register_over_length_key_returns_invalid_arg(void)
{
    bcs_reset();
    char long_key[BB_CACHE_KEY_MAX + 1];
    memset(long_key, 'k', sizeof(long_key) - 1);
    long_key[sizeof(long_key) - 1] = '\0';
    TEST_ASSERT_EQUAL(BB_CACHE_KEY_MAX, strlen(long_key));

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_cache_serialize_register(long_key, &s_bcs_desc, NULL, NULL));

    // Confirm no slot was claimed -- a get for the (truncated or full) key
    // is NOT_FOUND, not a stray binding.
    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                       bb_cache_serialize_get(BB_FORMAT_JSON, long_key, &out, &out_len));
}

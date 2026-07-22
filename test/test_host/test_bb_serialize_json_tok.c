// Host tests for the token-recorder sink + random-access accessors
// (bb_serialize_json_tok_*) -- the Stratum-message fixture set doubles as
// the worst-case-sizing proof (mining.notify w/ 16 merkle branches) and the
// zero-copy / arena-assembly contract regression.

#include "unity.h"
#include "bb_serialize_json.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POOL_CAP 64
#define ARENA_CAP 256

static bb_serialize_json_tok_t s_pool[POOL_CAP];
static char                    s_arena[ARENA_CAP];

static bb_err_t scan_into(bb_serialize_json_tok_recorder_t *rec, const char *doc,
                           bb_serialize_json_tok_t *pool, size_t pool_cap,
                           char *arena, size_t arena_cap)
{
    bb_err_t rc = bb_serialize_json_tok_recorder_init(rec, doc, strlen(doc), pool, pool_cap, arena, arena_cap);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    bb_serialize_json_ingest_t sink = bb_serialize_json_tok_recorder_ingest(rec);
    return bb_serialize_json_scan_bounded(doc, strlen(doc), &sink);
}

static bb_err_t scan_default(bb_serialize_json_tok_recorder_t *rec, const char *doc)
{
    return scan_into(rec, doc, s_pool, POOL_CAP, s_arena, ARENA_CAP);
}

static bb_serialize_json_tok_idx_t str_child(bb_serialize_json_tok_recorder_t *rec,
                                              bb_serialize_json_tok_idx_t obj, const char *key)
{
    return bb_serialize_json_tok_obj_get(rec, obj, key, strlen(key));
}

static void assert_str_eq(bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx,
                           const char *expected)
{
    const char *ptr = NULL;
    size_t      len = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_str(rec, idx, &ptr, &len));
    TEST_ASSERT_EQUAL(strlen(expected), len);
    TEST_ASSERT_EQUAL_MEMORY(expected, ptr, len);
}

// ---------------------------------------------------------------------------
// Zero-copy proof (escape-free string): the returned ptr must be a slice of
// the caller's own `doc` buffer -- pointer identity, not just content.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_tok_zero_copy_str_points_into_buf(void)
{
    bb_serialize_json_tok_recorder_t rec;
    char                              doc[] = "{\"method\":\"mining.subscribe\"}";
    TEST_ASSERT_EQUAL(BB_OK, scan_into(&rec, doc, s_pool, POOL_CAP, NULL, 0));

    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t m = str_child(&rec, root, "method");
    TEST_ASSERT_NOT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, m);

    const char *ptr = NULL;
    size_t      len = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_str(&rec, m, &ptr, &len));
    TEST_ASSERT_TRUE(ptr >= doc && ptr + len <= doc + sizeof(doc));
    TEST_ASSERT_EQUAL_STRING_LEN("mining.subscribe", ptr, len);
}

// ---------------------------------------------------------------------------
// Arena path: an escaped string assembles across alternating
// stable/scratch/stable chunks, lands OUTSIDE buf and INSIDE arena.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_tok_arena_assembles_alternating_chunks(void)
{
    bb_serialize_json_tok_recorder_t rec;
    char                              doc[] = "{\"x\":\"ab\\ncd\"}";
    TEST_ASSERT_EQUAL(BB_OK, scan_into(&rec, doc, s_pool, POOL_CAP, s_arena, ARENA_CAP));

    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t x = str_child(&rec, root, "x");
    TEST_ASSERT_NOT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, x);

    const char *ptr = NULL;
    size_t      len = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_str(&rec, x, &ptr, &len));
    TEST_ASSERT_EQUAL(5, len);
    TEST_ASSERT_EQUAL_MEMORY("ab\ncd", ptr, len);
    TEST_ASSERT_TRUE(ptr >= s_arena && ptr + len <= s_arena + ARENA_CAP);
    bool ptr_inside_doc = ptr >= doc && ptr < doc + sizeof(doc);
    TEST_ASSERT_FALSE(ptr_inside_doc);
}

// ---------------------------------------------------------------------------
// Arena absent / too small -> BB_ERR_NO_SPACE, no dangling pointer recorded.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_tok_arena_absent_no_space(void)
{
    bb_serialize_json_tok_recorder_t rec;
    const char                       *doc = "{\"x\":\"ab\\ncd\"}";
    bb_err_t rc = scan_into(&rec, doc, s_pool, POOL_CAP, NULL, 0);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);

    // No token for "x" was ever finalized with a dangling arena pointer --
    // the in-progress token exists (allocated before the copy failed) but
    // its str payload was never written, so get_str() on it must fail
    // rather than hand back garbage.
    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t x = str_child(&rec, root, "x");
    TEST_ASSERT_NOT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, x);
    const char *ptr = NULL;
    size_t      len = 0;
    // Type IS TOK_STR (allocated before the failed copy), but get_str()
    // still only reports what was actually written -- since the failure
    // happened before v.str was ever set, this must not read as a valid
    // non-NULL/garbage pointer beyond the token's zero-initialized state.
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_str(&rec, x, &ptr, &len));
    TEST_ASSERT_NULL(ptr);
    TEST_ASSERT_EQUAL(0, len);
}

void test_bb_serialize_json_tok_arena_too_small_no_space(void)
{
    bb_serialize_json_tok_recorder_t rec;
    char                              small_arena[2];  // "ab\ncd" needs 5 bytes
    const char                       *doc = "{\"x\":\"ab\\ncd\"}";
    bb_err_t rc = scan_into(&rec, doc, s_pool, POOL_CAP, small_arena, sizeof(small_arena));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

// ---------------------------------------------------------------------------
// Stratum fixture set
// ---------------------------------------------------------------------------

// mining.notify with a configurable merkle-branch count (0, 1, 16) and an
// optional trailing clean_jobs element -- the exact shape that determines
// the worst-case token-count arithmetic.
static void build_notify(char *out, size_t out_cap, int branch_count, bool include_clean_jobs)
{
    size_t n = 0;
    n += (size_t)snprintf(out + n, out_cap - n,
        "{\"id\":null,\"method\":\"mining.notify\",\"params\":["
        "\"job1\",\"prevhash\",\"coinb1\",\"coinb2\",[");
    for (int i = 0; i < branch_count; i++) {
        n += (size_t)snprintf(out + n, out_cap - n, "%s\"branch%02d\"", i > 0 ? "," : "", i);
    }
    n += (size_t)snprintf(out + n, out_cap - n, "],\"20000000\",\"1a44b9f2\",\"5e9a1b2c\"");
    if (include_clean_jobs) {
        n += (size_t)snprintf(out + n, out_cap - n, ",true");
    }
    snprintf(out + n, out_cap - n, "]}");
}

void test_bb_serialize_json_tok_notify_zero_branches_no_clean_jobs(void)
{
    char doc[512];
    build_notify(doc, sizeof(doc), 0, false);
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, doc));

    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    TEST_ASSERT_TRUE(bb_serialize_json_tok_is_obj(&rec, root));
    TEST_ASSERT_TRUE(bb_serialize_json_tok_is_null(&rec, str_child(&rec, root, "id")));
    assert_str_eq(&rec, str_child(&rec, root, "method"), "mining.notify");

    bb_serialize_json_tok_idx_t params = str_child(&rec, root, "params");
    TEST_ASSERT_TRUE(bb_serialize_json_tok_is_arr(&rec, params));
    TEST_ASSERT_EQUAL(8, bb_serialize_json_tok_arr_size(&rec, params));  // no clean_jobs

    bb_serialize_json_tok_idx_t branches = bb_serialize_json_tok_arr_at(&rec, params, 4);
    TEST_ASSERT_TRUE(bb_serialize_json_tok_is_arr(&rec, branches));
    TEST_ASSERT_EQUAL(0, bb_serialize_json_tok_arr_size(&rec, branches));
}

void test_bb_serialize_json_tok_notify_one_branch(void)
{
    char doc[512];
    build_notify(doc, sizeof(doc), 1, true);
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, doc));

    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t params = str_child(&rec, root, "params");
    TEST_ASSERT_EQUAL(9, bb_serialize_json_tok_arr_size(&rec, params));  // clean_jobs present

    bb_serialize_json_tok_idx_t branches = bb_serialize_json_tok_arr_at(&rec, params, 4);
    TEST_ASSERT_EQUAL(1, bb_serialize_json_tok_arr_size(&rec, branches));
    assert_str_eq(&rec, bb_serialize_json_tok_arr_at(&rec, branches, 0), "branch00");
}

// The worst-case fixture: 16 merkle branches + clean_jobs present. Exact
// token arithmetic (see bb_serialize_json.h's
// BB_SERIALIZE_JSON_TOK_POOL_DEFAULT_CAP doc comment): 1 (root obj) + 1 (id)
// + 1 (method) + 1 (params arr) + 4 (job_id/prevhash/coinb1/coinb2) + 1
// (merkle_branch arr) + 16 (branch strings) + 3 (version/nbits/ntime) + 1
// (clean_jobs) = 29 tokens.
void test_bb_serialize_json_tok_notify_16_branches_worst_case_token_count(void)
{
    char doc[2048];
    build_notify(doc, sizeof(doc), 16, true);
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_into(&rec, doc, s_pool, 29, s_arena, ARENA_CAP));

    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t params = str_child(&rec, root, "params");
    bb_serialize_json_tok_idx_t branches = bb_serialize_json_tok_arr_at(&rec, params, 4);
    TEST_ASSERT_EQUAL(16, bb_serialize_json_tok_arr_size(&rec, branches));
    assert_str_eq(&rec, bb_serialize_json_tok_arr_at(&rec, branches, 15), "branch15");

    bb_serialize_json_tok_idx_t clean_jobs = bb_serialize_json_tok_arr_at(&rec, params, 8);
    bool                        v = false;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_bool(&rec, clean_jobs, &v));
    TEST_ASSERT_TRUE(v);
}

// Pool exhaustion: exactly one token short of the full 16-branch notify.
void test_bb_serialize_json_tok_pool_exhaustion_one_short_of_notify(void)
{
    char doc[2048];
    build_notify(doc, sizeof(doc), 16, true);
    bb_serialize_json_tok_recorder_t rec;
    bb_err_t rc = scan_into(&rec, doc, s_pool, 28, s_arena, ARENA_CAP);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

void test_bb_serialize_json_tok_pool_exhaustion_generic(void)
{
    bb_serialize_json_tok_t          tiny_pool[2];
    bb_serialize_json_tok_recorder_t rec;
    bb_err_t rc = scan_into(&rec, "{\"a\":1,\"b\":2}", tiny_pool, 2, NULL, 0);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

// ---------------------------------------------------------------------------
// clean_jobs: absent (params length 8) vs present-and-false (length 9) --
// the two must be distinguishable via lookup + accessor behavior, not just
// array length.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_tok_clean_jobs_absent_vs_present_false(void)
{
    char doc_absent[512];
    char doc_false[512];
    build_notify(doc_absent, sizeof(doc_absent), 0, false);
    // Build a present-and-false variant manually (build_notify only emits
    // `true`).
    snprintf(doc_false, sizeof(doc_false),
        "{\"id\":null,\"method\":\"mining.notify\",\"params\":["
        "\"job1\",\"prevhash\",\"coinb1\",\"coinb2\",[],"
        "\"20000000\",\"1a44b9f2\",\"5e9a1b2c\",false]}");

    bb_serialize_json_tok_recorder_t rec_absent, rec_false;
    TEST_ASSERT_EQUAL(BB_OK, scan_into(&rec_absent, doc_absent, s_pool, POOL_CAP, s_arena, ARENA_CAP));

    bb_serialize_json_tok_t          pool2[POOL_CAP];
    char                              arena2[ARENA_CAP];
    TEST_ASSERT_EQUAL(BB_OK, scan_into(&rec_false, doc_false, pool2, POOL_CAP, arena2, ARENA_CAP));

    // Absent: params has 8 elements, index 8 (clean_jobs) is out of range.
    bb_serialize_json_tok_idx_t root_a = bb_serialize_json_tok_root(&rec_absent);
    bb_serialize_json_tok_idx_t params_a = str_child(&rec_absent, root_a, "params");
    TEST_ASSERT_EQUAL(8, bb_serialize_json_tok_arr_size(&rec_absent, params_a));
    bb_serialize_json_tok_idx_t cj_a = bb_serialize_json_tok_arr_at(&rec_absent, params_a, 8);
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, cj_a);
    bool out = true;  // pre-set to a sentinel value distinct from "not touched"
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_bool(&rec_absent, cj_a, &out));
    TEST_ASSERT_TRUE(out);  // untouched -- proves "safe no-op" contract

    // Present-and-false: params has 9 elements, index 8 is a real BOOL token.
    bb_serialize_json_tok_idx_t root_f = bb_serialize_json_tok_root(&rec_false);
    bb_serialize_json_tok_idx_t params_f = str_child(&rec_false, root_f, "params");
    TEST_ASSERT_EQUAL(9, bb_serialize_json_tok_arr_size(&rec_false, params_f));
    bb_serialize_json_tok_idx_t cj_f = bb_serialize_json_tok_arr_at(&rec_false, params_f, 8);
    TEST_ASSERT_NOT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, cj_f);
    bool out2 = true;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_bool(&rec_false, cj_f, &out2));
    TEST_ASSERT_FALSE(out2);
}

// mining.set_difficulty -- params[0] a number. "512.0" has a fraction, so
// get_f64() is the correct accessor for it; get_i64() must refuse (B1-1164)
// rather than hand back a strtoll()-truncated 512 -- covered generically by
// the num_inexact table in test_bb_serialize_json_tok_num_exact.c, asserted
// here too since this is the real Stratum shape that motivated it.
void test_bb_serialize_json_tok_set_difficulty(void)
{
    const char *doc = "{\"id\":null,\"method\":\"mining.set_difficulty\",\"params\":[512.0]}";
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, doc));

    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t params = str_child(&rec, root, "params");
    bb_serialize_json_tok_idx_t diff = bb_serialize_json_tok_arr_at(&rec, params, 0);
    double                      f = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_f64(&rec, diff, &f));
    TEST_ASSERT_EQUAL_DOUBLE(512.0, f);
    int64_t i = -1;
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_i64(&rec, diff, &i));
    TEST_ASSERT_EQUAL_INT64(-1, i);  // untouched -- safe no-op contract
}

// mining.subscribe result -- top-level ARRAY, [1] str, [2] int.
void test_bb_serialize_json_tok_subscribe_result(void)
{
    const char *doc = "[[[\"mining.set_difficulty\",\"deadbeef\"]],\"extranonce1abc\",4]";
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, doc));

    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    TEST_ASSERT_TRUE(bb_serialize_json_tok_is_arr(&rec, root));
    TEST_ASSERT_EQUAL(3, bb_serialize_json_tok_arr_size(&rec, root));
    assert_str_eq(&rec, bb_serialize_json_tok_arr_at(&rec, root, 1), "extranonce1abc");
    int64_t i = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_i64(&rec, bb_serialize_json_tok_arr_at(&rec, root, 2), &i));
    TEST_ASSERT_EQUAL_INT64(4, i);
}

// mining.authorize result -- bare bool at the root.
void test_bb_serialize_json_tok_authorize_result(void)
{
    const char *doc = "true";
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, doc));

    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bool                        v = false;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_bool(&rec, root, &v));
    TEST_ASSERT_TRUE(v);
}

// mining.set_extranonce -- params [str,int].
void test_bb_serialize_json_tok_set_extranonce(void)
{
    const char *doc = "{\"id\":null,\"method\":\"mining.set_extranonce\",\"params\":[\"deadbeef\",4]}";
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, doc));

    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t params = str_child(&rec, root, "params");
    assert_str_eq(&rec, bb_serialize_json_tok_arr_at(&rec, params, 0), "deadbeef");
    int64_t i = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_i64(&rec, bb_serialize_json_tok_arr_at(&rec, params, 1), &i));
    TEST_ASSERT_EQUAL_INT64(4, i);
}

// mining.configure result -- object with "version-rolling" bool +
// "version-rolling.mask" hex string (exercises the 20-byte key -- the
// longest across the fixture set, with 4 bytes of headroom to
// BB_SERIALIZE_JSON_TOK_KEY_MAX_LEN).
void test_bb_serialize_json_tok_configure_result(void)
{
    const char *doc = "{\"version-rolling\":true,\"version-rolling.mask\":\"1fffe000\"}";
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, doc));

    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bool                        v = false;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_bool(&rec, str_child(&rec, root, "version-rolling"), &v));
    TEST_ASSERT_TRUE(v);
    assert_str_eq(&rec, str_child(&rec, root, "version-rolling.mask"), "1fffe000");
}

// An error ARRAY (Stratum-style [code, message, traceback]).
void test_bb_serialize_json_tok_error_array(void)
{
    const char *doc = "[20,\"Other/Unknown\",null]";
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, doc));

    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    TEST_ASSERT_TRUE(bb_serialize_json_tok_is_arr(&rec, root));
    TEST_ASSERT_EQUAL(3, bb_serialize_json_tok_arr_size(&rec, root));
    int64_t code = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_i64(&rec, bb_serialize_json_tok_arr_at(&rec, root, 0), &code));
    TEST_ASSERT_EQUAL_INT64(20, code);
    assert_str_eq(&rec, bb_serialize_json_tok_arr_at(&rec, root, 1), "Other/Unknown");
    TEST_ASSERT_TRUE(bb_serialize_json_tok_is_null(&rec, bb_serialize_json_tok_arr_at(&rec, root, 2)));
}

// An error OBJECT.
void test_bb_serialize_json_tok_error_object(void)
{
    const char *doc = "{\"code\":20,\"message\":\"Other/Unknown\"}";
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, doc));

    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    int64_t code = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_i64(&rec, str_child(&rec, root, "code"), &code));
    TEST_ASSERT_EQUAL_INT64(20, code);
    assert_str_eq(&rec, str_child(&rec, root, "message"), "Other/Unknown");
}

// ---------------------------------------------------------------------------
// Accessors against the absent sentinel and against the wrong token type.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_tok_accessors_absent_sentinel_are_safe_noops(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{}"));

    bb_serialize_json_tok_idx_t absent = BB_SERIALIZE_JSON_TOK_ABSENT;
    TEST_ASSERT_FALSE(bb_serialize_json_tok_is_obj(&rec, absent));
    TEST_ASSERT_FALSE(bb_serialize_json_tok_is_arr(&rec, absent));
    TEST_ASSERT_FALSE(bb_serialize_json_tok_is_str(&rec, absent));
    TEST_ASSERT_FALSE(bb_serialize_json_tok_is_num(&rec, absent));
    TEST_ASSERT_FALSE(bb_serialize_json_tok_is_bool(&rec, absent));
    TEST_ASSERT_FALSE(bb_serialize_json_tok_is_null(&rec, absent));

    const char *ptr = (const char *)0x1;
    size_t      len = 999;
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_str(&rec, absent, &ptr, &len));
    TEST_ASSERT_EQUAL_PTR((const char *)0x1, ptr);
    TEST_ASSERT_EQUAL(999, len);

    int64_t i = -1;
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_i64(&rec, absent, &i));
    TEST_ASSERT_EQUAL_INT64(-1, i);

    double f = -1.0;
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_f64(&rec, absent, &f));
    TEST_ASSERT_EQUAL_DOUBLE(-1.0, f);

    bool b = true;
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_bool(&rec, absent, &b));
    TEST_ASSERT_TRUE(b);

    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT,
                       bb_serialize_json_tok_obj_get(&rec, absent, "x", 1));
    TEST_ASSERT_EQUAL(-1, bb_serialize_json_tok_arr_size(&rec, absent));
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, bb_serialize_json_tok_arr_at(&rec, absent, 0));
}

void test_bb_serialize_json_tok_accessors_wrong_type_are_safe_noops(void)
{
    const char *doc = "{\"o\":{},\"a\":[],\"s\":\"x\",\"n\":1,\"b\":true,\"z\":null}";
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, doc));

    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t o = str_child(&rec, root, "o");
    bb_serialize_json_tok_idx_t a = str_child(&rec, root, "a");
    bb_serialize_json_tok_idx_t s = str_child(&rec, root, "s");
    bb_serialize_json_tok_idx_t n = str_child(&rec, root, "n");
    bb_serialize_json_tok_idx_t b = str_child(&rec, root, "b");
    bb_serialize_json_tok_idx_t z = str_child(&rec, root, "z");

    // get_str only succeeds on TOK_STR.
    const char *ptr = NULL;
    size_t      len = 0;
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_str(&rec, o, &ptr, &len));
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_str(&rec, n, &ptr, &len));
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_str(&rec, b, &ptr, &len));
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_str(&rec, z, &ptr, &len));

    // get_i64/get_f64 only succeed on TOK_NUM.
    int64_t i = 0;
    double  f = 0;
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_i64(&rec, s, &i));
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_f64(&rec, s, &f));
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_i64(&rec, b, &i));

    // get_bool only succeeds on TOK_BOOL.
    bool v = false;
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_bool(&rec, n, &v));
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_bool(&rec, z, &v));

    // is_null only true on TOK_NULL.
    TEST_ASSERT_FALSE(bb_serialize_json_tok_is_null(&rec, o));
    TEST_ASSERT_FALSE(bb_serialize_json_tok_is_null(&rec, b));

    // obj_get on a non-OBJ token, arr_size/arr_at on a non-ARR token.
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, bb_serialize_json_tok_obj_get(&rec, a, "x", 1));
    TEST_ASSERT_EQUAL(-1, bb_serialize_json_tok_arr_size(&rec, o));
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, bb_serialize_json_tok_arr_at(&rec, o, 0));

    // A key that doesn't exist.
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, bb_serialize_json_tok_obj_get(&rec, root, "nope", 4));

    // An out-of-range array index.
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, bb_serialize_json_tok_arr_at(&rec, a, 0));
}

// ---------------------------------------------------------------------------
// Structural misuse guard: this sink must never be wired to the streaming
// entry points -- BB_SERIALIZE_JSON_SPAN_CALLER_FEED_SCOPED provenance
// (which only ever occurs there) is rejected outright.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_tok_rejects_streaming_provenance(void)
{
    bb_serialize_json_tok_recorder_t rec;
    const char                       *doc = "\"hello\"";
    TEST_ASSERT_EQUAL(BB_OK,
        bb_serialize_json_tok_recorder_init(&rec, doc, strlen(doc), s_pool, POOL_CAP, s_arena, ARENA_CAP));
    bb_serialize_json_ingest_t sink = bb_serialize_json_tok_recorder_ingest(&rec);

    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &sink));
    bb_err_t rc = bb_serialize_json_scan_feed(&ctx, doc, strlen(doc));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
}

// ---------------------------------------------------------------------------
// bb_serialize_json_tok_recorder_init() argument validation.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_tok_recorder_init_invalid_args(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_serialize_json_tok_recorder_init(NULL, "x", 1, s_pool, POOL_CAP, NULL, 0));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_serialize_json_tok_recorder_init(&rec, NULL, 0, s_pool, POOL_CAP, NULL, 0));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_serialize_json_tok_recorder_init(&rec, "x", 1, NULL, POOL_CAP, NULL, 0));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_serialize_json_tok_recorder_init(&rec, "x", 1, s_pool, 0, NULL, 0));
}

// ---------------------------------------------------------------------------
// root() before any scan / on an empty recorder.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_tok_root_before_scan_is_absent(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_tok_recorder_init(&rec, "x", 1, s_pool, POOL_CAP, NULL, 0));
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, bb_serialize_json_tok_root(&rec));
}

void test_bb_serialize_json_tok_root_null_rec_is_absent(void)
{
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, bb_serialize_json_tok_root(NULL));
}

// ---------------------------------------------------------------------------
// Key overflow -- a key longer than BB_SERIALIZE_JSON_TOK_KEY_MAX_LEN.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_tok_key_overflow_no_space(void)
{
    char doc[128];
    snprintf(doc, sizeof(doc), "{\"%.*s\":1}",
             (int)(BB_SERIALIZE_JSON_TOK_KEY_MAX_LEN + 1), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    bb_serialize_json_tok_recorder_t rec;
    bb_err_t rc = scan_default(&rec, doc);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

// A key of EXACTLY BB_SERIALIZE_JSON_TOK_KEY_MAX_LEN bytes is an exact fit,
// not an overflow -- distinct from the (KEY_MAX_LEN + 1) overflow case above.
void test_bb_serialize_json_tok_key_exact_boundary_success(void)
{
    char doc[128];
    char key[BB_SERIALIZE_JSON_TOK_KEY_MAX_LEN + 1];
    memset(key, 'a', BB_SERIALIZE_JSON_TOK_KEY_MAX_LEN);
    key[BB_SERIALIZE_JSON_TOK_KEY_MAX_LEN] = '\0';
    snprintf(doc, sizeof(doc), "{\"%s\":1}", key);

    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, doc));
    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t v = bb_serialize_json_tok_obj_get(&rec, root, key, strlen(key));
    TEST_ASSERT_NOT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, v);
    int64_t i = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_i64(&rec, v, &i));
    TEST_ASSERT_EQUAL_INT64(1, i);
}

// ---------------------------------------------------------------------------
// Branch-coverage fill.
// ---------------------------------------------------------------------------

// tok_valid()'s three-clause AND: NULL rec, negative idx (BB_SERIALIZE_JSON_TOK_ABSENT,
// covered by the absent-sentinel tests above), and an in-range-but-too-large
// positive idx (one past the last recorded token).
void test_bb_serialize_json_tok_is_x_null_rec_false(void)
{
    TEST_ASSERT_FALSE(bb_serialize_json_tok_is_obj(NULL, 0));
}

void test_bb_serialize_json_tok_is_x_idx_at_pool_n_out_of_range_false(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"a\":1}"));
    // Exactly 2 tokens recorded (root + "a"); index 2 is one past the end.
    TEST_ASSERT_FALSE(bb_serialize_json_tok_is_obj(&rec, 2));
}

// An empty-string object key ("") -- the scanner's cur_key() collapses a
// zero-length key to a NULL pointer (see bb_serialize_json_scan.c), so this
// member is recorded exactly like an unkeyed value (key_len == 0), yet must
// still be findable via bb_serialize_json_tok_obj_get(obj, "", 0).
void test_bb_serialize_json_tok_empty_key(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"\":1}"));
    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t v = bb_serialize_json_tok_obj_get(&rec, root, "", 0);
    TEST_ASSERT_NOT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, v);
    int64_t i = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_i64(&rec, v, &i));
    TEST_ASSERT_EQUAL_INT64(1, i);
}

// Pool exhaustion striking exactly at each callback's own tok_alloc() call
// site (rather than a call site already exercised by another test).
void test_bb_serialize_json_tok_pool_exhaustion_at_begin_obj(void)
{
    bb_serialize_json_tok_t          pool[1];
    bb_serialize_json_tok_recorder_t rec;
    // Root object (slot 0) succeeds; the nested {} for "a" needs slot 1.
    bb_err_t rc = scan_into(&rec, "{\"a\":{}}", pool, 1, NULL, 0);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

void test_bb_serialize_json_tok_pool_exhaustion_at_begin_arr(void)
{
    bb_serialize_json_tok_t          pool[1];
    bb_serialize_json_tok_recorder_t rec;
    // Root array (slot 0) succeeds; the nested [] needs slot 1.
    bb_err_t rc = scan_into(&rec, "[[]]", pool, 1, NULL, 0);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

void test_bb_serialize_json_tok_pool_exhaustion_at_value_null(void)
{
    bb_serialize_json_tok_t          pool[1];
    bb_serialize_json_tok_recorder_t rec;
    // Root object (slot 0) succeeds; the null value for "a" needs slot 1.
    bb_err_t rc = scan_into(&rec, "{\"a\":null}", pool, 1, NULL, 0);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

void test_bb_serialize_json_tok_pool_exhaustion_at_str_chunk_first_alloc(void)
{
    bb_serialize_json_tok_t          pool[1];
    bb_serialize_json_tok_recorder_t rec;
    // Root object (slot 0) succeeds; the string value for "a" needs slot 1
    // -- fails at value_str_chunk's own tok_alloc(), before the zero-copy
    // vs. arena decision is even made.
    bb_err_t rc = scan_into(&rec, "{\"a\":\"x\"}", pool, 1, NULL, 0);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

// tok_value_str_chunk's `is_final && provenance == CALLER_STABLE` combo:
// a string that is ENTIRELY a single escape (e.g. "\n") is_final=true on
// its one and only call, but provenance is SCANNER_SCRATCH -- the (true,
// false) combo, distinct from the zero-copy (true, true) case covered by
// test_bb_serialize_json_tok_zero_copy_str_points_into_buf.
void test_bb_serialize_json_tok_str_first_call_final_scratch(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"x\":\"\\n\"}"));
    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    assert_str_eq(&rec, str_child(&rec, root, "x"), "\n");
}

// The (false, false) combo: a string that STARTS with an escape but is not
// yet complete on that first call (more direct-span bytes follow) --
// is_final=false, provenance=SCANNER_SCRATCH on the first value_str_chunk
// call.
void test_bb_serialize_json_tok_str_first_call_nonfinal_scratch(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"x\":\"\\nab\"}"));
    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    assert_str_eq(&rec, str_child(&rec, root, "x"), "\nab");
}

// An empty string VALUE ("") -- the exact zero-length-call edge case
// (chunk_len==0, is_final=true, CALLER_STABLE) that the arena's
// `chunk_len > 0` exclusion (see tok_value_str_chunk in
// bb_serialize_json_tok.c) relies on being the ONLY zero-length call the
// scanner ever makes -- assert it explicitly rather than only inferring it
// from that exclusion's reachability argument.
//
// Scans with arena = NULL, arena_cap = 0 -- which makes the arena branch
// UNREACHABLE (any write there would fail on rec->arena == NULL) -- so a
// non-zero `len` here can only come from the zero-copy fast path, and the
// pointer-identity assertion below (mirroring
// test_bb_serialize_json_tok_zero_copy_str_points_into_buf) is load-bearing:
// scan_default()'s own arena would otherwise let a regressed fast path (one
// that fell through to the arena branch) still pass on `len == 0` alone,
// since arena+0 with len==0 looks identical to the zero-copy ptr==doc+0.
void test_bb_serialize_json_tok_empty_string_value(void)
{
    bb_serialize_json_tok_recorder_t rec;
    char                              doc[] = "{\"x\":\"\"}";
    TEST_ASSERT_EQUAL(BB_OK, scan_into(&rec, doc, s_pool, POOL_CAP, NULL, 0));
    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    const char                 *ptr = NULL;
    size_t                       len = 999;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_str(&rec, str_child(&rec, root, "x"), &ptr, &len));
    TEST_ASSERT_EQUAL(0, len);
    TEST_ASSERT_TRUE(ptr >= doc && ptr <= doc + sizeof(doc));
}

// bb_serialize_json_tok_obj_get() must SKIP a nested container's own
// descendants (a different, deeper parent) while scanning for a direct
// child -- not just linearly match the first `child_count` tokens after
// `obj`.
void test_bb_serialize_json_tok_obj_get_skips_nested_descendants(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"a\":{\"nested\":1},\"b\":2}"));
    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t b = str_child(&rec, root, "b");
    TEST_ASSERT_NOT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, b);
    int64_t i = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_i64(&rec, b, &i));
    TEST_ASSERT_EQUAL_INT64(2, i);
}

// bb_serialize_json_tok_obj_get()'s scan loop must stop as soon as all
// `child_count` direct children have been seen, even when the pool has MORE
// tokens after them (a later sibling of the queried object's own parent) --
// exercises the for-loop's `remaining > 0` condition going false while
// `i < rec->pool_n` is still true (as opposed to the loop instead running
// off the end of the pool).
void test_bb_serialize_json_tok_obj_get_stops_at_child_count_not_pool_end(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"a\":{\"x\":1,\"y\":2},\"b\":3}"));
    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t a = str_child(&rec, root, "a");
    bb_serialize_json_tok_idx_t y = str_child(&rec, a, "y");
    TEST_ASSERT_NOT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, y);
    int64_t i = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_i64(&rec, y, &i));
    TEST_ASSERT_EQUAL_INT64(2, i);
}

// A failed lookup (key not found) whose object's children are exhausted
// (remaining hits 0) BEFORE the scan reaches the end of the pool (there is
// a later, unrelated sibling token still in the pool) -- exercises the
// for-loop exiting via `remaining > 0` going false rather than via
// `i < rec->pool_n` running out.
void test_bb_serialize_json_tok_obj_get_not_found_exhausts_children_before_pool_end(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"a\":{\"x\":1,\"y\":2},\"b\":3}"));
    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t a = str_child(&rec, root, "a");
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_TOK_ABSENT, bb_serialize_json_tok_obj_get(&rec, a, "nope", 4));
}

// Scalar accessors with NULL out-params -- must still report found/not-found
// via the return value without dereferencing a NULL out pointer.
void test_bb_serialize_json_tok_get_str_null_out_params(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"x\":\"y\"}"));
    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t x = str_child(&rec, root, "x");
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_str(&rec, x, NULL, NULL));
}

void test_bb_serialize_json_tok_get_i64_null_out_param(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"x\":1}"));
    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t x = str_child(&rec, root, "x");
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_i64(&rec, x, NULL));
}

void test_bb_serialize_json_tok_get_f64_null_out_param(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"x\":1.5}"));
    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t x = str_child(&rec, root, "x");
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_f64(&rec, x, NULL));
}

void test_bb_serialize_json_tok_get_bool_null_out_param(void)
{
    bb_serialize_json_tok_recorder_t rec;
    TEST_ASSERT_EQUAL(BB_OK, scan_default(&rec, "{\"x\":true}"));
    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(&rec);
    bb_serialize_json_tok_idx_t x = str_child(&rec, root, "x");
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_bool(&rec, x, NULL));
}

// ---------------------------------------------------------------------------
// child_count overflow guard: a container already at UINT16_MAX direct
// children must fail the NEXT allocation with BB_ERR_NO_SPACE rather than
// silently wrapping child_count to 0 (which would corrupt
// obj_get()/arr_size()/arr_at() navigation with no error signal).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// B1-1164: get_i64()'s num_inexact refusal -- a fraction ('.') or exponent
// ('e'/'E') in the number's raw text means get_i64() must return false
// rather than hand back a strtoll()-truncated value; get_f64() is always
// unaffected. See tok_parse_num()'s doc comment in bb_serialize_json_tok.c
// for why this is derived from the raw text rather than from t->v.num.f64.
// ---------------------------------------------------------------------------

static bb_serialize_json_tok_idx_t num_child(bb_serialize_json_tok_recorder_t *rec, const char *num_text)
{
    char doc[128];
    snprintf(doc, sizeof(doc), "{\"n\":%s}", num_text);
    TEST_ASSERT_EQUAL(BB_OK, scan_default(rec, doc));
    bb_serialize_json_tok_idx_t root = bb_serialize_json_tok_root(rec);
    return str_child(rec, root, "n");
}

void test_bb_serialize_json_tok_num_exact_exponent_form_refuses_i64(void)
{
    bb_serialize_json_tok_recorder_t rec;
    bb_serialize_json_tok_idx_t      n = num_child(&rec, "5e1");
    int64_t                          i = -1;
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_i64(&rec, n, &i));
    TEST_ASSERT_EQUAL_INT64(-1, i);
}

void test_bb_serialize_json_tok_num_exact_uppercase_exponent_form_refuses_i64(void)
{
    bb_serialize_json_tok_recorder_t rec;
    bb_serialize_json_tok_idx_t      n = num_child(&rec, "5E1");
    int64_t                          i = -1;
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_i64(&rec, n, &i));
    TEST_ASSERT_EQUAL_INT64(-1, i);
    double f = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_f64(&rec, n, &f));
    TEST_ASSERT_EQUAL_DOUBLE(50.0, f);
}

void test_bb_serialize_json_tok_num_exact_fraction_plus_exponent_refuses_i64(void)
{
    bb_serialize_json_tok_recorder_t rec;
    bb_serialize_json_tok_idx_t      n = num_child(&rec, "1.5e10");
    int64_t                          i = -1;
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_i64(&rec, n, &i));
    TEST_ASSERT_EQUAL_INT64(-1, i);
}

void test_bb_serialize_json_tok_num_exact_large_exponent_refuses_i64_but_f64_still_works(void)
{
    bb_serialize_json_tok_recorder_t rec;
    bb_serialize_json_tok_idx_t      n = num_child(&rec, "1e300");
    int64_t                          i = -1;
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_i64(&rec, n, &i));
    TEST_ASSERT_EQUAL_INT64(-1, i);
    double f = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_f64(&rec, n, &f));
    TEST_ASSERT_EQUAL_DOUBLE(1e300, f);
}

void test_bb_serialize_json_tok_num_exact_fraction_refuses_i64(void)
{
    bb_serialize_json_tok_recorder_t rec;
    bb_serialize_json_tok_idx_t      n = num_child(&rec, "1.9");
    int64_t                          i = -1;
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_i64(&rec, n, &i));
    TEST_ASSERT_EQUAL_INT64(-1, i);
}

void test_bb_serialize_json_tok_num_exact_negative_int(void)
{
    bb_serialize_json_tok_recorder_t rec;
    bb_serialize_json_tok_idx_t      n = num_child(&rec, "-5");
    int64_t                          i = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_i64(&rec, n, &i));
    TEST_ASSERT_EQUAL_INT64(-5, i);
}

void test_bb_serialize_json_tok_num_exact_zero(void)
{
    bb_serialize_json_tok_recorder_t rec;
    bb_serialize_json_tok_idx_t      n = num_child(&rec, "0");
    int64_t                          i = -1;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_i64(&rec, n, &i));
    TEST_ASSERT_EQUAL_INT64(0, i);
}

void test_bb_serialize_json_tok_num_exact_uint32_max(void)
{
    bb_serialize_json_tok_recorder_t rec;
    bb_serialize_json_tok_idx_t      n = num_child(&rec, "4294967295");
    int64_t                          i = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_i64(&rec, n, &i));
    TEST_ASSERT_EQUAL_INT64(4294967295LL, i);
}

void test_bb_serialize_json_tok_num_exact_uint32_max_plus_one(void)
{
    bb_serialize_json_tok_recorder_t rec;
    bb_serialize_json_tok_idx_t      n = num_child(&rec, "4294967296");
    int64_t                          i = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_i64(&rec, n, &i));
    TEST_ASSERT_EQUAL_INT64(4294967296LL, i);
}

// 20 digits, no fraction/exponent -- out of int64_t range. strtoll()
// saturates to INT64_MAX per C -- unchanged today, num_inexact is NOT set
// for a plain (if overflowing) digit run, only for '.'/'e'/'E' forms.
void test_bb_serialize_json_tok_num_exact_plain_digit_overflow_saturates_unchanged(void)
{
    bb_serialize_json_tok_recorder_t rec;
    bb_serialize_json_tok_idx_t      n = num_child(&rec, "99999999999999999999");
    int64_t                          i = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_i64(&rec, n, &i));
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, i);
}

// 2^53 + 1, plain digits, no fraction/exponent -- exactly representable in
// int64_t but NOT exactly representable as a double (53-bit mantissa).
// This is the regression test that catches an accidental option-(c)
// implementation (deriving i64 from f64): get_i64() must return the exact
// strtoll()-parsed value, not a value rounded through a double.
void test_bb_serialize_json_tok_num_exact_above_double_mantissa_precision_still_exact(void)
{
    bb_serialize_json_tok_recorder_t rec;
    bb_serialize_json_tok_idx_t      n = num_child(&rec, "9007199254740993");
    int64_t                          i = 0;
    TEST_ASSERT_TRUE(bb_serialize_json_tok_get_i64(&rec, n, &i));
    TEST_ASSERT_EQUAL_INT64(9007199254740993LL, i);
}

void test_bb_serialize_json_tok_num_exact_int64_max_negative_exponent_refuses_i64(void)
{
    bb_serialize_json_tok_recorder_t rec;
    bb_serialize_json_tok_idx_t      n = num_child(&rec, "9223372036854775807e-18");
    int64_t                          i = -1;
    TEST_ASSERT_FALSE(bb_serialize_json_tok_get_i64(&rec, n, &i));
    TEST_ASSERT_EQUAL_INT64(-1, i);
}

void test_bb_serialize_json_tok_child_count_wrap_guard(void)
{
    const size_t n_elems = (size_t)UINT16_MAX + 1; // one past the ceiling
    size_t       doc_cap = n_elems * 2 + 4;         // "[" + "0," per elem + "]" + NUL
    char        *doc = malloc(doc_cap);
    TEST_ASSERT_NOT_NULL(doc);
    size_t off = 0;
    doc[off++] = '[';
    for (size_t i = 0; i < n_elems; i++) {
        doc[off++] = '0';
        if (i + 1 < n_elems) doc[off++] = ',';
    }
    doc[off++] = ']';
    doc[off] = '\0';

    size_t                    pool_cap = n_elems + 1; // root array + n_elems numbers; plenty of room
    bb_serialize_json_tok_t  *pool = malloc(pool_cap * sizeof(*pool));
    TEST_ASSERT_NOT_NULL(pool);

    bb_serialize_json_tok_recorder_t rec;
    bb_err_t rc = scan_into(&rec, doc, pool, pool_cap, NULL, 0);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);

    free(doc);
    free(pool);
}

#include "unity.h"
#include "bb_http_server.h"
#include "bb_core.h"
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Stub handler functions — used only for pointer comparison, never called.
 * ---------------------------------------------------------------------------*/
static bb_err_t handler_get_x(bb_http_request_t *req)  { (void)req; return BB_OK; }
static bb_err_t handler_post_x(bb_http_request_t *req) { (void)req; return BB_OK; }
static bb_err_t handler_other(bb_http_request_t *req)  { (void)req; return BB_OK; }

/* setUp calls bb_dispatch_api_reset() so each test starts with a clean table. */

/* ---------------------------------------------------------------------------
 * add + lookup HIT: right handler returned
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_add_and_lookup_hit(void)
{
    bb_dispatch_api_reset();
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x", handler_get_x));
    TEST_ASSERT_EQUAL(1, (int)bb_dispatch_api_count());

    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/x", &out);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT, res);
    TEST_ASSERT_EQUAL_PTR(handler_get_x, out);
}

/* ---------------------------------------------------------------------------
 * lookup MISS: unknown path
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_lookup_miss_unknown_path(void)
{
    bb_dispatch_api_reset();
    bb_dispatch_api_add(BB_HTTP_GET, "/api/x", handler_get_x);

    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/z", &out);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_MISS, res);
    TEST_ASSERT_NULL(out);
}

/* ---------------------------------------------------------------------------
 * METHOD_MISMATCH: add GET /api/x, look up POST /api/x → 405 path
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_method_mismatch(void)
{
    bb_dispatch_api_reset();
    bb_dispatch_api_add(BB_HTTP_GET, "/api/x", handler_get_x);

    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_POST, "/api/x", &out);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_METHOD_MISMATCH, res);
    /* out_handler must remain unchanged on non-HIT */
    TEST_ASSERT_NULL(out);
}

/* ---------------------------------------------------------------------------
 * Method discrimination: GET and POST on same path return distinct handlers
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_method_discrimination(void)
{
    bb_dispatch_api_reset();
    bb_dispatch_api_add(BB_HTTP_GET,  "/api/x", handler_get_x);
    bb_dispatch_api_add(BB_HTTP_POST, "/api/x", handler_post_x);
    TEST_ASSERT_EQUAL(2, (int)bb_dispatch_api_count());

    bb_http_handler_fn out_get = NULL;
    bb_dispatch_api_result_t res_get =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/x", &out_get);
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT, res_get);
    TEST_ASSERT_EQUAL_PTR(handler_get_x, out_get);

    bb_http_handler_fn out_post = NULL;
    bb_dispatch_api_result_t res_post =
        bb_dispatch_api_lookup(BB_HTTP_POST, "/api/x", &out_post);
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT, res_post);
    TEST_ASSERT_EQUAL_PTR(handler_post_x, out_post);
}

/* ---------------------------------------------------------------------------
 * Query-string stripping: "/api/x?foo=bar" matches "/api/x"
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_query_string_stripped(void)
{
    bb_dispatch_api_reset();
    bb_dispatch_api_add(BB_HTTP_GET, "/api/x", handler_get_x);

    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/x?foo=bar", &out);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT, res);
    TEST_ASSERT_EQUAL_PTR(handler_get_x, out);
}

/* ---------------------------------------------------------------------------
 * Exact (not prefix): "/api/x" must NOT match a lookup for "/api/xy"
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_exact_not_prefix_longer_uri(void)
{
    bb_dispatch_api_reset();
    bb_dispatch_api_add(BB_HTTP_GET, "/api/x", handler_get_x);

    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/xy", &out);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_MISS, res);
}

/* ---------------------------------------------------------------------------
 * Exact (not prefix): "/api/xy" must NOT match a lookup for "/api/x"
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_exact_not_prefix_shorter_uri(void)
{
    bb_dispatch_api_reset();
    bb_dispatch_api_add(BB_HTTP_GET, "/api/xy", handler_get_x);

    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/x", &out);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_MISS, res);
}

/* ---------------------------------------------------------------------------
 * Non-/api URI → MISS
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_non_api_uri_miss(void)
{
    bb_dispatch_api_reset();
    bb_dispatch_api_add(BB_HTTP_GET, "/api/x", handler_get_x);

    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/assets/main.js", &out);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_MISS, res);
}

/* ---------------------------------------------------------------------------
 * Overflow: fill to CAP, next add returns BB_ERR_NO_SPACE, lookups for the
 * dropped route MISS, no crash.
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_overflow_returns_no_space(void)
{
    bb_dispatch_api_reset();

    /* Fill the table with distinct paths /api/fill0 … /api/fill<CAP-1>
     * so the dup-detection scan does not drop any entries. */
    static const char *overflow_path = "/api/overflow";
    static char fill_paths[BB_DISPATCH_API_CAP][24];

    for (int i = 0; i < BB_DISPATCH_API_CAP; i++) {
        snprintf(fill_paths[i], sizeof(fill_paths[i]), "/api/fill%d", i);
        bb_err_t err = bb_dispatch_api_add(BB_HTTP_GET, fill_paths[i],
                                           handler_other);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_CAP, (int)bb_dispatch_api_count());

    /* The CAP+1-th add must fail non-fatally. */
    bb_err_t err = bb_dispatch_api_add(BB_HTTP_GET, overflow_path,
                                       handler_other);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);

    /* Count must not exceed cap. */
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_CAP, (int)bb_dispatch_api_count());

    /* Lookup of the dropped route returns MISS. */
    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, overflow_path, &out);
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_MISS, res);
}

/* ---------------------------------------------------------------------------
 * reset() clears the table — count returns 0
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_reset_clears(void)
{
    bb_dispatch_api_reset();
    bb_dispatch_api_add(BB_HTTP_GET, "/api/x", handler_get_x);
    TEST_ASSERT_EQUAL(1, (int)bb_dispatch_api_count());

    bb_dispatch_api_reset();
    TEST_ASSERT_EQUAL(0, (int)bb_dispatch_api_count());

    /* Lookup after reset returns MISS, not garbage. */
    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/x", &out);
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_MISS, res);
}

/* ---------------------------------------------------------------------------
 * NULL uri → MISS (defensive)
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_null_uri_returns_miss(void)
{
    bb_dispatch_api_reset();
    bb_dispatch_api_add(BB_HTTP_GET, "/api/x", handler_get_x);

    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, NULL, &out);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_MISS, res);
}

/* ---------------------------------------------------------------------------
 * NULL out_handler → MISS (defensive)
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_null_out_handler_returns_miss(void)
{
    bb_dispatch_api_reset();
    bb_dispatch_api_add(BB_HTTP_GET, "/api/x", handler_get_x);

    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/x", NULL);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_MISS, res);
}

/* ---------------------------------------------------------------------------
 * NULL path stored via add → lookup skips that entry (defensive guard)
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_null_path_entry_skipped(void)
{
    bb_dispatch_api_reset();
    /* Store a NULL path; the lookup must not crash and must return MISS. */
    bb_dispatch_api_add(BB_HTTP_GET, NULL, handler_get_x);
    TEST_ASSERT_EQUAL(1, (int)bb_dispatch_api_count());

    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/x", &out);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_MISS, res);
    TEST_ASSERT_NULL(out);
}

/* ---------------------------------------------------------------------------
 * Duplicate (method,path): second add is dropped, count unchanged, first
 * handler still dispatches.
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_dup_same_method_and_path_dropped(void)
{
    bb_dispatch_api_reset();
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x", handler_get_x));
    TEST_ASSERT_EQUAL(1, (int)bb_dispatch_api_count());

    /* Second registration of the same (method, path) must return non-OK
     * and leave the count unchanged. */
    bb_err_t err = bb_dispatch_api_add(BB_HTTP_GET, "/api/x", handler_other);
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(1, (int)bb_dispatch_api_count());

    /* First handler must still be the one that dispatches. */
    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/x", &out);
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT, res);
    TEST_ASSERT_EQUAL_PTR(handler_get_x, out);
}

/* ---------------------------------------------------------------------------
 * Duplicate check is NOT triggered when method differs — same path, different
 * method must both be kept (e.g. GET and POST on the same path are not dups).
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_dup_different_method_same_path_both_kept(void)
{
    bb_dispatch_api_reset();
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x", handler_get_x));
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_POST, "/api/x", handler_post_x));
    TEST_ASSERT_EQUAL(2, (int)bb_dispatch_api_count());

    bb_http_handler_fn out_get = NULL;
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT,
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/x", &out_get));
    TEST_ASSERT_EQUAL_PTR(handler_get_x, out_get);

    bb_http_handler_fn out_post = NULL;
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT,
        bb_dispatch_api_lookup(BB_HTTP_POST, "/api/x", &out_post));
    TEST_ASSERT_EQUAL_PTR(handler_post_x, out_post);
}

/* ---------------------------------------------------------------------------
 * NULL path does not participate in dup-detection scan — both adds succeed
 * without crashing.  (NULL paths are already covered elsewhere for lookup
 * safety; this guards the new scan loop against NULL entries.)
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_dup_null_path_not_dup_detected(void)
{
    bb_dispatch_api_reset();
    /* First add with NULL path. */
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, NULL, handler_get_x));
    /* Second add with NULL path — must not false-positive as a dup. */
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, NULL, handler_post_x));
    TEST_ASSERT_EQUAL(2, (int)bb_dispatch_api_count());
}

/* ---------------------------------------------------------------------------
 * Dup scan skips existing NULL-path entries when the incoming path is
 * non-NULL — exercises the `s_dispatch[i].path == NULL` continue branch
 * inside the dup-detection loop.
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_dup_scan_skips_null_path_existing_entry(void)
{
    bb_dispatch_api_reset();
    /* Pre-populate with a NULL-path entry so the scan encounters it. */
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, NULL, handler_get_x));

    /* Add a non-NULL path — dup scan must traverse (and skip) the NULL entry
     * without crashing, and this add must succeed. */
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x", handler_post_x));
    TEST_ASSERT_EQUAL(2, (int)bb_dispatch_api_count());

    /* The non-NULL route must be reachable. */
    bb_http_handler_fn out = NULL;
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT,
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/x", &out));
    TEST_ASSERT_EQUAL_PTR(handler_post_x, out);
}

/* ---------------------------------------------------------------------------
 * High-watermark warn: adding CAP-8 entries triggers the warn branch;
 * the (CAP-7)th add still succeeds.
 * ---------------------------------------------------------------------------*/
void test_dispatch_api_high_watermark_warn(void)
{
    bb_dispatch_api_reset();

    /* Add CAP-8 entries with distinct paths — watermark fires on the last one.
     * Using distinct paths ensures the dup-detection scan does not drop any. */
    static char wm_paths[BB_DISPATCH_API_CAP][24];

    for (int i = 0; i < BB_DISPATCH_API_CAP - 8; i++) {
        snprintf(wm_paths[i], sizeof(wm_paths[i]), "/api/wm%d", i);
        TEST_ASSERT_EQUAL(BB_OK,
            bb_dispatch_api_add(BB_HTTP_GET, wm_paths[i], handler_other));
    }
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_CAP - 8, (int)bb_dispatch_api_count());

    /* The next 8 adds must still succeed (warn was informational).
     * Use POST to avoid method collision with the GET entries above on shared path. */
    for (int i = 0; i < 8; i++) {
        snprintf(wm_paths[BB_DISPATCH_API_CAP - 8 + i],
                 sizeof(wm_paths[0]), "/api/wm%d", BB_DISPATCH_API_CAP - 8 + i);
        TEST_ASSERT_EQUAL(BB_OK,
            bb_dispatch_api_add(BB_HTTP_POST,
                                wm_paths[BB_DISPATCH_API_CAP - 8 + i],
                                handler_other));
    }
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_CAP, (int)bb_dispatch_api_count());
}

/* ---------------------------------------------------------------------------
 * Wildcard routing — scoped-prefix dispatch
 * ---------------------------------------------------------------------------*/

/* Exact wins over a same-prefix wildcard — wildcard registered FIRST. */
void test_dispatch_api_exact_wins_over_wildcard_wildcard_first(void)
{
    bb_dispatch_api_reset();
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x/*", handler_other));
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x/y", handler_get_x));

    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/x/y", &out);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT, res);
    TEST_ASSERT_EQUAL_PTR(handler_get_x, out);
}

/* Exact wins over a same-prefix wildcard — exact registered FIRST. */
void test_dispatch_api_exact_wins_over_wildcard_exact_first(void)
{
    bb_dispatch_api_reset();
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x/y", handler_get_x));
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x/*", handler_other));

    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/x/y", &out);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT, res);
    TEST_ASSERT_EQUAL_PTR(handler_get_x, out);
}

/* Longest-prefix wins across two registered wildcards. */
void test_dispatch_api_wildcard_longest_prefix_wins(void)
{
    bb_dispatch_api_reset();
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x/*", handler_get_x));
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x/y/*", handler_other));

    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/x/y/z", &out);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT, res);
    TEST_ASSERT_EQUAL_PTR(handler_other, out);
}

/* Longest-prefix wins regardless of registration order (broader added last). */
void test_dispatch_api_wildcard_longest_prefix_wins_reverse_order(void)
{
    bb_dispatch_api_reset();
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x/y/*", handler_other));
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x/*", handler_get_x));

    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/x/y/z", &out);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT, res);
    TEST_ASSERT_EQUAL_PTR(handler_other, out);
}

/* Wildcard MISS: uri doesn't match any registered prefix -> 404 path. */
void test_dispatch_api_wildcard_miss(void)
{
    bb_dispatch_api_reset();
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x/*", handler_get_x));

    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/z/y", &out);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_MISS, res);
    TEST_ASSERT_NULL(out);
}

/* Wildcard prefix hit but wrong method -> METHOD_MISMATCH (405 path). */
void test_dispatch_api_wildcard_method_mismatch(void)
{
    bb_dispatch_api_reset();
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x/*", handler_get_x));

    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_POST, "/api/x/y", &out);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_METHOD_MISMATCH, res);
    TEST_ASSERT_NULL(out);
}

/* Exact-path-but-method-mismatch still returns METHOD_MISMATCH even when a
 * broader wildcard would have matched — the exact route claims the path. */
void test_dispatch_api_exact_method_mismatch_not_rescued_by_wildcard(void)
{
    bb_dispatch_api_reset();
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_POST, "/api/x/y", handler_get_x));
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x/*", handler_other));

    bb_http_handler_fn out = NULL;
    bb_dispatch_api_result_t res =
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/x/y", &out);

    TEST_ASSERT_EQUAL(BB_DISPATCH_API_METHOD_MISMATCH, res);
    TEST_ASSERT_NULL(out);
}

/* Regression: every existing exact /api/* route still resolves correctly
 * with a wildcard entry also present. */
void test_dispatch_api_exact_routes_unaffected_by_wildcard_presence(void)
{
    bb_dispatch_api_reset();
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/a", handler_get_x));
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_POST, "/api/b", handler_post_x));
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/wild/*", handler_other));

    bb_http_handler_fn out_a = NULL;
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT,
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/a", &out_a));
    TEST_ASSERT_EQUAL_PTR(handler_get_x, out_a);

    bb_http_handler_fn out_b = NULL;
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT,
        bb_dispatch_api_lookup(BB_HTTP_POST, "/api/b", &out_b));
    TEST_ASSERT_EQUAL_PTR(handler_post_x, out_b);

    bb_http_handler_fn out_wild = NULL;
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT,
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/wild/anything", &out_wild));
    TEST_ASSERT_EQUAL_PTR(handler_other, out_wild);
}

/* Tie-break on equal prefix_len between two same-path wildcards registered
 * under different methods (allowed — dup-detection only rejects identical
 * (method,path) pairs, see test_dispatch_api_dup_different_method_same_path_
 * both_kept). Exercises the `>` (not `>=`) tie-break in Pass 2: the
 * first-registered wildcard wins the tie even though a later, method-correct
 * wildcard registered at the same prefix would otherwise satisfy the
 * request — first registration wins, documented behavior. */
void test_dispatch_api_wildcard_tie_break_first_registered_wins(void)
{
    bb_dispatch_api_reset();
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x/*", handler_get_x));
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_POST, "/api/x/*", handler_post_x));
    TEST_ASSERT_EQUAL(2, (int)bb_dispatch_api_count());

    /* GET matches the first-registered entry directly -> HIT. */
    bb_http_handler_fn out_get = NULL;
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_HIT,
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/x/y", &out_get));
    TEST_ASSERT_EQUAL_PTR(handler_get_x, out_get);

    /* POST: the tie-break keeps the first-registered (GET) entry as "best"
     * since the POST entry's equal prefix_len does not beat it (`>`, not
     * `>=`), so the correctly-matching POST wildcard is never considered —
     * METHOD_MISMATCH, not HIT. */
    bb_http_handler_fn out_post = NULL;
    TEST_ASSERT_EQUAL(BB_DISPATCH_API_METHOD_MISMATCH,
        bb_dispatch_api_lookup(BB_HTTP_POST, "/api/x/y", &out_post));
    TEST_ASSERT_NULL(out_post);
}

/* Duplicate wildcard pattern (same method,path) is rejected the same way as
 * an exact duplicate. */
void test_dispatch_api_dup_wildcard_pattern_dropped(void)
{
    bb_dispatch_api_reset();
    TEST_ASSERT_EQUAL(BB_OK,
        bb_dispatch_api_add(BB_HTTP_GET, "/api/x/*", handler_get_x));
    TEST_ASSERT_EQUAL(1, (int)bb_dispatch_api_count());

    bb_err_t err = bb_dispatch_api_add(BB_HTTP_GET, "/api/x/*", handler_other);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    TEST_ASSERT_EQUAL(1, (int)bb_dispatch_api_count());
}

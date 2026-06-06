#include "unity.h"
#include "bb_http_api_dispatch.h"
#include "bb_core.h"

/* ---------------------------------------------------------------------------
 * Stub handler functions — used only for pointer comparison, never called.
 * ---------------------------------------------------------------------------*/
static bb_err_t handler_get_x(bb_http_request_t *req)  { (void)req; return BB_OK; }
static bb_err_t handler_post_x(bb_http_request_t *req) { (void)req; return BB_OK; }
static bb_err_t handler_other(bb_http_request_t *req)  { (void)req; return BB_OK; }

/* setUp calls bb_api_dispatch_reset() so each test starts with a clean table. */

/* ---------------------------------------------------------------------------
 * add + lookup HIT: right handler returned
 * ---------------------------------------------------------------------------*/
void test_api_dispatch_add_and_lookup_hit(void)
{
    bb_api_dispatch_reset();
    TEST_ASSERT_EQUAL(BB_OK,
        bb_api_dispatch_add(BB_HTTP_GET, "/api/x", handler_get_x));
    TEST_ASSERT_EQUAL(1, (int)bb_api_dispatch_count());

    bb_http_handler_fn out = NULL;
    bb_api_dispatch_result_t res =
        bb_api_dispatch_lookup(BB_HTTP_GET, "/api/x", &out);

    TEST_ASSERT_EQUAL(BB_API_DISPATCH_HIT, res);
    TEST_ASSERT_EQUAL_PTR(handler_get_x, out);
}

/* ---------------------------------------------------------------------------
 * lookup MISS: unknown path
 * ---------------------------------------------------------------------------*/
void test_api_dispatch_lookup_miss_unknown_path(void)
{
    bb_api_dispatch_reset();
    bb_api_dispatch_add(BB_HTTP_GET, "/api/x", handler_get_x);

    bb_http_handler_fn out = NULL;
    bb_api_dispatch_result_t res =
        bb_api_dispatch_lookup(BB_HTTP_GET, "/api/z", &out);

    TEST_ASSERT_EQUAL(BB_API_DISPATCH_MISS, res);
    TEST_ASSERT_NULL(out);
}

/* ---------------------------------------------------------------------------
 * METHOD_MISMATCH: add GET /api/x, look up POST /api/x → 405 path
 * ---------------------------------------------------------------------------*/
void test_api_dispatch_method_mismatch(void)
{
    bb_api_dispatch_reset();
    bb_api_dispatch_add(BB_HTTP_GET, "/api/x", handler_get_x);

    bb_http_handler_fn out = NULL;
    bb_api_dispatch_result_t res =
        bb_api_dispatch_lookup(BB_HTTP_POST, "/api/x", &out);

    TEST_ASSERT_EQUAL(BB_API_DISPATCH_METHOD_MISMATCH, res);
    /* out_handler must remain unchanged on non-HIT */
    TEST_ASSERT_NULL(out);
}

/* ---------------------------------------------------------------------------
 * Method discrimination: GET and POST on same path return distinct handlers
 * ---------------------------------------------------------------------------*/
void test_api_dispatch_method_discrimination(void)
{
    bb_api_dispatch_reset();
    bb_api_dispatch_add(BB_HTTP_GET,  "/api/x", handler_get_x);
    bb_api_dispatch_add(BB_HTTP_POST, "/api/x", handler_post_x);
    TEST_ASSERT_EQUAL(2, (int)bb_api_dispatch_count());

    bb_http_handler_fn out_get = NULL;
    bb_api_dispatch_result_t res_get =
        bb_api_dispatch_lookup(BB_HTTP_GET, "/api/x", &out_get);
    TEST_ASSERT_EQUAL(BB_API_DISPATCH_HIT, res_get);
    TEST_ASSERT_EQUAL_PTR(handler_get_x, out_get);

    bb_http_handler_fn out_post = NULL;
    bb_api_dispatch_result_t res_post =
        bb_api_dispatch_lookup(BB_HTTP_POST, "/api/x", &out_post);
    TEST_ASSERT_EQUAL(BB_API_DISPATCH_HIT, res_post);
    TEST_ASSERT_EQUAL_PTR(handler_post_x, out_post);
}

/* ---------------------------------------------------------------------------
 * Query-string stripping: "/api/x?foo=bar" matches "/api/x"
 * ---------------------------------------------------------------------------*/
void test_api_dispatch_query_string_stripped(void)
{
    bb_api_dispatch_reset();
    bb_api_dispatch_add(BB_HTTP_GET, "/api/x", handler_get_x);

    bb_http_handler_fn out = NULL;
    bb_api_dispatch_result_t res =
        bb_api_dispatch_lookup(BB_HTTP_GET, "/api/x?foo=bar", &out);

    TEST_ASSERT_EQUAL(BB_API_DISPATCH_HIT, res);
    TEST_ASSERT_EQUAL_PTR(handler_get_x, out);
}

/* ---------------------------------------------------------------------------
 * Exact (not prefix): "/api/x" must NOT match a lookup for "/api/xy"
 * ---------------------------------------------------------------------------*/
void test_api_dispatch_exact_not_prefix_longer_uri(void)
{
    bb_api_dispatch_reset();
    bb_api_dispatch_add(BB_HTTP_GET, "/api/x", handler_get_x);

    bb_http_handler_fn out = NULL;
    bb_api_dispatch_result_t res =
        bb_api_dispatch_lookup(BB_HTTP_GET, "/api/xy", &out);

    TEST_ASSERT_EQUAL(BB_API_DISPATCH_MISS, res);
}

/* ---------------------------------------------------------------------------
 * Exact (not prefix): "/api/xy" must NOT match a lookup for "/api/x"
 * ---------------------------------------------------------------------------*/
void test_api_dispatch_exact_not_prefix_shorter_uri(void)
{
    bb_api_dispatch_reset();
    bb_api_dispatch_add(BB_HTTP_GET, "/api/xy", handler_get_x);

    bb_http_handler_fn out = NULL;
    bb_api_dispatch_result_t res =
        bb_api_dispatch_lookup(BB_HTTP_GET, "/api/x", &out);

    TEST_ASSERT_EQUAL(BB_API_DISPATCH_MISS, res);
}

/* ---------------------------------------------------------------------------
 * Non-/api URI → MISS
 * ---------------------------------------------------------------------------*/
void test_api_dispatch_non_api_uri_miss(void)
{
    bb_api_dispatch_reset();
    bb_api_dispatch_add(BB_HTTP_GET, "/api/x", handler_get_x);

    bb_http_handler_fn out = NULL;
    bb_api_dispatch_result_t res =
        bb_api_dispatch_lookup(BB_HTTP_GET, "/assets/main.js", &out);

    TEST_ASSERT_EQUAL(BB_API_DISPATCH_MISS, res);
}

/* ---------------------------------------------------------------------------
 * Overflow: fill to CAP, next add returns BB_ERR_NO_SPACE, lookups for the
 * dropped route MISS, no crash.
 * ---------------------------------------------------------------------------*/
void test_api_dispatch_overflow_returns_no_space(void)
{
    bb_api_dispatch_reset();

    /* Fill the table with /api/0 … /api/<CAP-1> using static path storage.
     * All paths share the same static pointer — the test only cares that
     * the CAP-th add fails; the lookup MISS below uses a distinct literal. */
    static const char *overflow_path = "/api/overflow";

    for (int i = 0; i < BB_API_DISPATCH_CAP; i++) {
        bb_err_t err = bb_api_dispatch_add(BB_HTTP_GET, "/api/fill",
                                           handler_other);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }
    TEST_ASSERT_EQUAL(BB_API_DISPATCH_CAP, (int)bb_api_dispatch_count());

    /* The CAP+1-th add must fail non-fatally. */
    bb_err_t err = bb_api_dispatch_add(BB_HTTP_GET, overflow_path,
                                       handler_other);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);

    /* Count must not exceed cap. */
    TEST_ASSERT_EQUAL(BB_API_DISPATCH_CAP, (int)bb_api_dispatch_count());

    /* Lookup of the dropped route returns MISS. */
    bb_http_handler_fn out = NULL;
    bb_api_dispatch_result_t res =
        bb_api_dispatch_lookup(BB_HTTP_GET, overflow_path, &out);
    TEST_ASSERT_EQUAL(BB_API_DISPATCH_MISS, res);
}

/* ---------------------------------------------------------------------------
 * reset() clears the table — count returns 0
 * ---------------------------------------------------------------------------*/
void test_api_dispatch_reset_clears(void)
{
    bb_api_dispatch_reset();
    bb_api_dispatch_add(BB_HTTP_GET, "/api/x", handler_get_x);
    TEST_ASSERT_EQUAL(1, (int)bb_api_dispatch_count());

    bb_api_dispatch_reset();
    TEST_ASSERT_EQUAL(0, (int)bb_api_dispatch_count());

    /* Lookup after reset returns MISS, not garbage. */
    bb_http_handler_fn out = NULL;
    bb_api_dispatch_result_t res =
        bb_api_dispatch_lookup(BB_HTTP_GET, "/api/x", &out);
    TEST_ASSERT_EQUAL(BB_API_DISPATCH_MISS, res);
}

/* ---------------------------------------------------------------------------
 * NULL uri → MISS (defensive)
 * ---------------------------------------------------------------------------*/
void test_api_dispatch_null_uri_returns_miss(void)
{
    bb_api_dispatch_reset();
    bb_api_dispatch_add(BB_HTTP_GET, "/api/x", handler_get_x);

    bb_http_handler_fn out = NULL;
    bb_api_dispatch_result_t res =
        bb_api_dispatch_lookup(BB_HTTP_GET, NULL, &out);

    TEST_ASSERT_EQUAL(BB_API_DISPATCH_MISS, res);
}

/* ---------------------------------------------------------------------------
 * NULL out_handler → MISS (defensive)
 * ---------------------------------------------------------------------------*/
void test_api_dispatch_null_out_handler_returns_miss(void)
{
    bb_api_dispatch_reset();
    bb_api_dispatch_add(BB_HTTP_GET, "/api/x", handler_get_x);

    bb_api_dispatch_result_t res =
        bb_api_dispatch_lookup(BB_HTTP_GET, "/api/x", NULL);

    TEST_ASSERT_EQUAL(BB_API_DISPATCH_MISS, res);
}

/* ---------------------------------------------------------------------------
 * NULL path stored via add → lookup skips that entry (defensive guard)
 * ---------------------------------------------------------------------------*/
void test_api_dispatch_null_path_entry_skipped(void)
{
    bb_api_dispatch_reset();
    /* Store a NULL path; the lookup must not crash and must return MISS. */
    bb_api_dispatch_add(BB_HTTP_GET, NULL, handler_get_x);
    TEST_ASSERT_EQUAL(1, (int)bb_api_dispatch_count());

    bb_http_handler_fn out = NULL;
    bb_api_dispatch_result_t res =
        bb_api_dispatch_lookup(BB_HTTP_GET, "/api/x", &out);

    TEST_ASSERT_EQUAL(BB_API_DISPATCH_MISS, res);
    TEST_ASSERT_NULL(out);
}

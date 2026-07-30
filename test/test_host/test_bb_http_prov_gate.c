#include "unity.h"
#include "bb_http_prov_gate.h"
#include "bb_core.h"
#include <stdio.h>

// setUp() (test_main.c) calls bb_http_prov_gate_reset() before every test in
// this binary, so each test here starts with an empty allowlist without
// needing its own reset call (same convention as bb_dispatch_api_reset(),
// see test_dispatch_api.c).

// ---------------------------------------------------------------------------
// prov_active == false: gate never denies, regardless of allowlist state.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_inactive_always_allows(void)
{
    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(false, BB_HTTP_GET, "/api/diag/meminfo"));
}

// ---------------------------------------------------------------------------
// Empty allowlist + prov_active == true: everything is denied (structural
// default-deny).
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_empty_allowlist_denies(void)
{
    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/ping"));
}

// ---------------------------------------------------------------------------
// Exact-path match on the allowlisted method is allowed.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_exact_match_allows(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/ping"));

    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/ping"));
}

// ---------------------------------------------------------------------------
// Wildcard (suffix '*') match is allowed for any path under the prefix.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_wildcard_match_allows(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/prov/*"));

    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/prov/status"));
}

// ---------------------------------------------------------------------------
// Near-miss of a wildcard prefix must NOT match — the prefix comparison is
// byte-exact up to prefix_len, not a loose "starts with the stem" match.
// "/api/prov/*" has prefix "/api/prov/" (includes the trailing slash); a
// path missing that slash is a different, non-matching prefix.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_wildcard_near_miss_denies(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/prov/*"));

    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/provisioning"));
}

// ---------------------------------------------------------------------------
// Method mismatch on an allowlisted PATH still denies — the allowlist is
// per (method,path), not per path.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_method_mismatch_denies(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/ping"));

    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_POST, "/ping"));
}

// ---------------------------------------------------------------------------
// Query string is stripped before matching an exact entry, same convention
// as bb_dispatch_api_lookup.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_query_string_stripped(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/ping"));

    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/ping?x=1"));
}

// ---------------------------------------------------------------------------
// bb_http_prov_allow() is safe to call for a path never looked up elsewhere,
// and returns BB_ERR_INVALID_ARG on a NULL path.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_allow_null_path_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_prov_allow(BB_HTTP_GET, NULL));
}

// ---------------------------------------------------------------------------
// gate_allow() with a NULL uri denies rather than crashing.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_null_uri_denies(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/ping"));

    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, NULL));
}

// ---------------------------------------------------------------------------
// Cap overflow: fill the allowlist to BB_HTTP_PROV_ALLOWLIST_CAP, the next
// add returns BB_ERR_NO_SPACE, and the dropped entry stays denied (no crash,
// no silent grow).
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_overflow_returns_no_space(void)
{
    static char fill_paths[BB_HTTP_PROV_ALLOWLIST_CAP][24];

    for (int i = 0; i < BB_HTTP_PROV_ALLOWLIST_CAP; i++) {
        snprintf(fill_paths[i], sizeof(fill_paths[i]), "/api/fill%d", i);
        TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, fill_paths[i]));
    }

    static const char *overflow_path = "/api/overflow";
    bb_err_t err = bb_http_prov_allow(BB_HTTP_GET, overflow_path);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);

    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, overflow_path));
}

// ---------------------------------------------------------------------------
// reset() clears the allowlist — a previously-allowed exact path denies
// again after reset.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_reset_clears(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/ping"));
    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/ping"));

    bb_http_prov_gate_reset();

    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/ping"));
}

// ---------------------------------------------------------------------------
// Empty-string path is never a wildcard (plen == 0 branch of the trailing-'*'
// check) and still round-trips as an exact match.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_empty_path_not_wildcard(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, ""));

    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, ""));
}

// ---------------------------------------------------------------------------
// A uri shorter than a wildcard's prefix must short-circuit deny (the
// path_len < prefix_len branch) rather than reading past the uri's length.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_wildcard_prefix_longer_than_uri_denies(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/prov/*"));

    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api"));
}

// ---------------------------------------------------------------------------
// Exact-match candidate with the SAME length but different content must
// deny (the memcmp-mismatch branch), not fall through to an accidental
// match.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_exact_same_length_mismatch_denies(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/aaa"));

    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/bbb"));
}

// ---------------------------------------------------------------------------
// INTERIM restriction (HIGH1, header): a wildcard entry outside /api/ is
// rejected outright — bb_route_uri_match's subset predicate is only
// verified correct for the /api/* family bb_dispatch_api actually
// dispatches; non-/api routes go through ESP-IDF's own
// httpd_uri_match_wildcard, a matcher this gate never touches. Direction 1:
// a non-/api wildcard is rejected and never reaches the allowlist.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_wildcard_non_api_path_rejected(void)
{
    bb_err_t err = bb_http_prov_allow(BB_HTTP_GET, "/static/*");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);

    // Rejected entry never reaches the allowlist -- still denied even for a
    // uri that would otherwise match the (rejected) prefix.
    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/static/style.css"));
}

// ---------------------------------------------------------------------------
// Direction 2: a wildcard entry rooted at /api/ is accepted (the family the
// predicate is actually verified against) -- already exercised end-to-end
// by test_bb_http_prov_gate_wildcard_match_allows above; this test isolates
// just the bb_http_prov_allow() return value for the restriction check.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_wildcard_api_path_accepted(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/prov/*"));
}

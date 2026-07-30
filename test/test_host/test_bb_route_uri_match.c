#include "unity.h"
#include "../../components/bb_http_server/src/bb_route_match.h"

// These tests exercise ONLY the interim subset predicate that this seam
// currently implements (exact match, or a single trailing-'*' prefix
// match) plus its NULL/bounds guards -- see bb_route_match.h. They
// deliberately do NOT probe interior wildcards or '?' single-char
// patterns: this seam's stated target is to become a platform hook (real
// httpd_uri_match_wildcard on device, a trivial selection-only stub on
// host), so richer glob semantics are only validly testable on-device
// against the real matcher, never here.

// ---------------------------------------------------------------------------
// NULL guards
// ---------------------------------------------------------------------------
void test_bb_route_uri_match_null_pattern_false(void)
{
    TEST_ASSERT_FALSE(bb_route_uri_match(NULL, "/ping", 5));
}

void test_bb_route_uri_match_null_uri_false(void)
{
    TEST_ASSERT_FALSE(bb_route_uri_match("/ping", NULL, 5));
}

// ---------------------------------------------------------------------------
// Exact match (no trailing '*')
// ---------------------------------------------------------------------------
void test_bb_route_uri_match_exact_match_true(void)
{
    TEST_ASSERT_TRUE(bb_route_uri_match("/ping", "/ping", 5));
}

void test_bb_route_uri_match_exact_mismatch_false(void)
{
    // Same length (match_upto == strlen(pattern)) but different content.
    TEST_ASSERT_FALSE(bb_route_uri_match("/api/aaa", "/api/bbb", 8));
}

// ---------------------------------------------------------------------------
// Empty pattern ("" is never a wildcard -- plen > 0 is false, exact branch
// taken: plen(0) must equal match_upto).
// ---------------------------------------------------------------------------
void test_bb_route_uri_match_empty_pattern_zero_match_upto_true(void)
{
    TEST_ASSERT_TRUE(bb_route_uri_match("", "", 0));
}

void test_bb_route_uri_match_empty_pattern_nonzero_match_upto_false(void)
{
    TEST_ASSERT_FALSE(bb_route_uri_match("", "/x", 2));
}

// ---------------------------------------------------------------------------
// Suffix-wildcard prefix match
// ---------------------------------------------------------------------------
void test_bb_route_uri_match_wildcard_prefix_match_true(void)
{
    TEST_ASSERT_TRUE(bb_route_uri_match("/api/prov/*", "/api/prov/status", 16));
}

void test_bb_route_uri_match_wildcard_prefix_mismatch_false(void)
{
    // Same length as the pattern's prefix requirement, but the prefix bytes
    // differ ("/api/prof/..." vs "/api/prov/*").
    TEST_ASSERT_FALSE(bb_route_uri_match("/api/prov/*", "/api/prof/status", 16));
}

void test_bb_route_uri_match_wildcard_match_upto_shorter_than_prefix_false(void)
{
    // "/api/prov/*" has a 10-byte prefix ("/api/prov/"); match_upto=4 is
    // shorter, so the match must short-circuit deny rather than read past
    // match_upto.
    TEST_ASSERT_FALSE(bb_route_uri_match("/api/prov/*", "/api", 4));
}

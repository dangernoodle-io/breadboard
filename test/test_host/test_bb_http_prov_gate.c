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

// ---------------------------------------------------------------------------
// B1-1400: bb_http_prov_deny() validation.
// ---------------------------------------------------------------------------

void test_bb_http_prov_gate_deny_null_path_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_prov_deny(BB_HTTP_GET, NULL));
}

void test_bb_http_prov_gate_deny_wildcard_non_api_path_rejected(void)
{
    bb_err_t err = bb_http_prov_deny(BB_HTTP_GET, "/static/*");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_http_prov_gate_deny_overflow_returns_no_space(void)
{
    static char fill_paths[BB_HTTP_PROV_DENYLIST_CAP][24];

    for (int i = 0; i < BB_HTTP_PROV_DENYLIST_CAP; i++) {
        snprintf(fill_paths[i], sizeof(fill_paths[i]), "/api/fill%d", i);
        TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, fill_paths[i]));
    }

    static const char *overflow_path = "/api/overflow";
    bb_err_t err = bb_http_prov_deny(BB_HTTP_GET, overflow_path);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
}

// ---------------------------------------------------------------------------
// FAIL-OPEN CONTRACT (pins bb_http_prov_gate.h's bb_http_prov_deny()
// overflow doc): fill the denylist to capacity, then attempt a deny for a
// path P covered by an existing allow entry. The attempt overflows
// (BB_ERR_NO_SPACE), so P's deny entry never gets recorded — the gate must
// therefore leave P ALLOWED, the opposite direction from
// test_bb_http_prov_gate_deny_write_path_actually_gates_read_path's
// fail-closed allow-overflow pin above.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_deny_overflow_leaves_path_allowed(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/diag/*"));

    static char fill_paths[BB_HTTP_PROV_DENYLIST_CAP][24];

    for (int i = 0; i < BB_HTTP_PROV_DENYLIST_CAP; i++) {
        snprintf(fill_paths[i], sizeof(fill_paths[i]), "/api/fill%d", i);
        TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, fill_paths[i]));
    }

    bb_err_t err = bb_http_prov_deny(BB_HTTP_GET, "/api/diag/reboot");
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);

    // Overflowed deny never recorded -- the covering allow entry still
    // reaches this path, so it fails OPEN.
    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/diag/reboot"));
}

void test_bb_http_prov_gate_deny_success(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, "/api/diag/reboot"));
}

void test_bb_http_prov_gate_deny_empty_path_accepted(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, ""));
}

// ---------------------------------------------------------------------------
// Direction 2 of the wildcard restriction (mirrors
// test_bb_http_prov_gate_wildcard_api_path_accepted for allow): a wildcard
// deny entry rooted at /api/ is accepted -- covers the is_wildcard==true &&
// wildcard_in_api_scope==true branch of bb_http_prov_deny()'s guard.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_deny_wildcard_api_path_accepted(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, "/api/prov/*"));
}

// ---------------------------------------------------------------------------
// Regression guard: an allow match with no deny entries at all still
// allows — unchanged from pre-B1-1400 behaviour.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_allow_no_deny_entries_allows(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/ping"));

    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/ping"));
}

// ---------------------------------------------------------------------------
// An exact deny entry on the same (method,path) as an allow match wins —
// deny always suppresses allow.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_exact_deny_suppresses_exact_allow(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/diag/reboot"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, "/api/diag/reboot"));

    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/diag/reboot"));
}

// ---------------------------------------------------------------------------
// THE B1-1279 MOTIVATING CASE: a broad wildcard allow with a narrow exact
// deny carve-out. The carved-out path denies; every other path under the
// wildcard still allows.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_deny_carves_out_wildcard_allow(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/diag/*"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, "/api/diag/reboot"));

    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/diag/status"));
    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/diag/reboot"));
}

// ---------------------------------------------------------------------------
// A deny entry on a DIFFERENT method from the request must NOT suppress —
// deny is per (method,path), same granularity as allow.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_deny_method_mismatch_does_not_suppress(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/diag/reboot"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_POST, "/api/diag/reboot"));

    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/diag/reboot"));
}

// ---------------------------------------------------------------------------
// Order independence: registering deny-then-allow or allow-then-deny must
// produce identical results — deny is a post-filter over the resolved
// allow union, not order-sensitive, mirroring the allow-vs-allow guarantee
// bb_http_prov_gate.h documents.
//
// NOTE: this guarantee is STRUCTURAL, not behavioural — s_allowlist[] and
// s_denylist[] are two separate, exhaustively-scanned tables with no
// shared index space, so registration order literally cannot affect which
// entries exist in which table. These two tests exercise that there is no
// accidental cross-table ordering dependency in the call sites below, but
// they are not evidence of some other last-match-wins resolution scheme
// being absent — there is no code path capable of expressing one.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_deny_then_allow_order_independent(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, "/api/diag/reboot"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/diag/*"));

    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/diag/status"));
    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/diag/reboot"));
}

void test_bb_http_prov_gate_allow_then_deny_order_independent(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/diag/*"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, "/api/diag/reboot"));

    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/diag/status"));
    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/diag/reboot"));
}

// ---------------------------------------------------------------------------
// verify_denies: an orphaned deny entry (no allow entry could ever reach
// it) fails with BB_ERR_NOT_FOUND.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_verify_denies_orphan_returns_not_found(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/diag/*"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, "/api/other/reboot"));

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_http_prov_gate_verify_denies());
}

// ---------------------------------------------------------------------------
// verify_denies: a deny entry reachable by a matching allow entry passes.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_verify_denies_covered_returns_ok(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/diag/*"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, "/api/diag/reboot"));

    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_gate_verify_denies());
}

// ---------------------------------------------------------------------------
// verify_denies: an allow entry on a DIFFERENT method must be skipped
// (the inner loop's method-mismatch continue branch) rather than
// considered a match -- reachability is only ever found via the later,
// same-method allow entry.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_verify_denies_skips_method_mismatched_allow(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_POST, "/api/diag/*"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/diag/*"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, "/api/diag/reboot"));

    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_gate_verify_denies());
}

// ---------------------------------------------------------------------------
// verify_denies: an empty denylist (the common case — no consumer ever
// calls bb_http_prov_deny()) trivially passes.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_verify_denies_empty_returns_ok(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_gate_verify_denies());
}

// ---------------------------------------------------------------------------
// HIGH-1 REGRESSION: a wildcard deny under an EXACT allow must be reported
// covered, not orphaned. The old literal-uri-match implementation always
// matched the allow entry's path as a "pattern" against the deny entry's
// own text (including its trailing '*') — a shape that only happened to
// work when the DENY side, not the allow side, carried the wildcard. This
// is the shape the bug actually shipped with: allow = exact
// "/api/diag/reboot", deny = wildcard "/api/diag/*", which the gate
// correctly denies at runtime (bb_http_prov_gate_allow() below) but the
// old verify_denies() reported BB_ERR_NOT_FOUND for.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_verify_denies_wildcard_deny_vs_exact_allow_covered(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/diag/reboot"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, "/api/diag/*"));

    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_gate_verify_denies());

    // Confirm the gate's live enforcement agrees with verify_denies()'s
    // "covered" verdict — the deny is not merely reachable on paper, it
    // actually gates this exact request.
    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/diag/reboot"));
}

// ---------------------------------------------------------------------------
// A wildcard deny BROADER than a wildcard allow (deny's prefix is a strict
// prefix of allow's, the opposite of test_..._deny_carves_out_wildcard_allow
// above) must also be reported covered — any URI matching the narrower
// allow also matches the broader deny.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_verify_denies_wildcard_deny_broader_than_wildcard_allow_covered(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/diag/foo*"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, "/api/diag/*"));

    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_gate_verify_denies());
}

// ---------------------------------------------------------------------------
// A genuinely orphaned WILDCARD deny — its prefix does not overlap any
// allow entry's matched-URI set at all — must still fail with
// BB_ERR_NOT_FOUND after the rewrite, same as the pre-existing exact-deny
// orphan case.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_verify_denies_wildcard_deny_orphan_returns_not_found(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/prov/*"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, "/api/diag/*"));

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_http_prov_gate_verify_denies());
}

// ---------------------------------------------------------------------------
// (b) exact deny vs exact allow: an EXACT deny entry identical to an EXACT
// allow entry is covered. verify_denies_covered_returns_ok above exercises
// exact-deny-under-WILDCARD-allow, not this exact-vs-exact shape — this
// test isolates the exact-vs-exact branch of
// bb_http_prov_gate_entries_overlap() (both operands non-wildcard).
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_verify_denies_exact_deny_vs_exact_allow_covered(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/diag/reboot"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, "/api/diag/reboot"));

    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_gate_verify_denies());
}

// ---------------------------------------------------------------------------
// (b) exact deny vs exact allow, same-length mismatch: two exact entries of
// identical length but different content must NOT overlap — exercises the
// memcmp-mismatch half of the exact-vs-exact branch's short-circuit.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_verify_denies_exact_vs_exact_same_length_mismatch_orphan(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/diag/reboot"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, "/api/diag/status"));

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_http_prov_gate_verify_denies());
}

// ---------------------------------------------------------------------------
// (b) exact deny vs exact allow, length mismatch: two exact entries of
// different lengths must NOT overlap even when one is a byte-for-byte
// prefix of the other — exact entries never get prefix semantics, only
// wildcard entries do. Exercises the length-inequality short-circuit of the
// exact-vs-exact branch.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_verify_denies_exact_vs_exact_length_mismatch_orphan(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/diag/reboot"));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_deny(BB_HTTP_GET, "/api/diag/rebootx"));

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_http_prov_gate_verify_denies());
}

// ---------------------------------------------------------------------------
// NON-NEGOTIABLE (mechanism-fires proof): bb_http_prov_deny() returning
// BB_OK on the write path is only meaningful if the read path
// (bb_http_prov_gate_allow()) actually consults the denylist. This test
// asserts BOTH: the write succeeds AND the gate subsequently denies the
// exact path just recorded. If the post-filter loop in
// bb_http_prov_gate_allow() were ever left unreachable (e.g. dead code, an
// early return before it runs), bb_http_prov_deny() would still report
// BB_OK while the gate wrongly allowed — this test is the one that catches
// exactly that disconnect.
// ---------------------------------------------------------------------------
void test_bb_http_prov_gate_deny_write_path_actually_gates_read_path(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_http_prov_allow(BB_HTTP_GET, "/api/diag/*"));

    // Before deny: the gate allows (sanity baseline).
    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/diag/reboot"));

    bb_err_t deny_result = bb_http_prov_deny(BB_HTTP_GET, "/api/diag/reboot");
    TEST_ASSERT_EQUAL(BB_OK, deny_result);

    // After deny: the write path reported success, so the read path MUST
    // now reflect it.
    TEST_ASSERT_FALSE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/diag/reboot"));
}

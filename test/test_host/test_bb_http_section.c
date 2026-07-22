// Host tests for the bb_http_section dispatch helper (bb_http_section PR):
// bb_http_section_register_ns() / bb_http_section_find() /
// bb_http_section_count()/_at() (the portable registry+lookup, priv header)
// plus bb_http_section_status_for_render()/_for_apply() (the pure
// err->status mapping, mirrors cache_route_status.h/
// bb_wifi_http_apply_status.h). Also proves the helper is genuinely usable
// end to end (not dead code) by wiring a fake namespace's apply() hook
// straight onto bb_data_parse()/bb_data_commit() against a real bb_data
// binding -- the SAME two calls a production bb_data-backed namespace (a
// later PR's real consumer) would use, without converting any existing
// route onto the helper in this PR.

#include "unity.h"

#include "bb_http_section_priv.h"
#include "bb_http_section_status.h"

#include "bb_data.h"
#include "bb_serialize_format.h"
#include "bb_serialize_json.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

static void hs_reset(void)
{
    bb_http_section_test_reset();
}

static bb_err_t hs_render_ok(const char *name, const bb_serialize_query_t *query,
                              char *buf, size_t cap, size_t *out_len, void *ctx)
{
    (void)name;
    (void)query;
    (void)ctx;
    const char *body = "{\"ok\":true}";
    size_t      n     = strlen(body);
    if (n >= cap) return BB_ERR_NO_SPACE;
    memcpy(buf, body, n);
    *out_len = n;
    return BB_OK;
}

static bb_http_section_apply_result_t hs_apply_ok(const char *name, const char *body,
                                                   size_t body_len, void *ctx)
{
    (void)name;
    (void)body;
    (void)body_len;
    (void)ctx;
    return (bb_http_section_apply_result_t){ .stage = BB_HTTP_SECTION_STAGE_COMMIT, .rc = BB_OK };
}

// ---------------------------------------------------------------------------
// bb_http_section_register_ns
// ---------------------------------------------------------------------------

void test_bb_http_section_register_ns_success(void)
{
    hs_reset();
    bb_http_section_ns_t ns = { .prefix = "/api/hs.ok/", .render = hs_render_ok, .apply = hs_apply_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_http_section_register_ns(&ns));
    TEST_ASSERT_EQUAL(1, bb_http_section_count());
}

void test_bb_http_section_register_ns_render_only_ok(void)
{
    hs_reset();
    bb_http_section_ns_t ns = { .prefix = "/api/hs.renderonly/", .render = hs_render_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_http_section_register_ns(&ns));
}

void test_bb_http_section_register_ns_apply_only_ok(void)
{
    hs_reset();
    bb_http_section_ns_t ns = { .prefix = "/api/hs.applyonly/", .apply = hs_apply_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_http_section_register_ns(&ns));
}

void test_bb_http_section_register_ns_null_ns_returns_invalid_arg(void)
{
    hs_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_section_register_ns(NULL));
}

void test_bb_http_section_register_ns_null_prefix_returns_invalid_arg(void)
{
    hs_reset();
    bb_http_section_ns_t ns = { .prefix = NULL, .render = hs_render_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_section_register_ns(&ns));
}

void test_bb_http_section_register_ns_both_hooks_null_returns_invalid_arg(void)
{
    hs_reset();
    bb_http_section_ns_t ns = { .prefix = "/api/hs.useless/" };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_section_register_ns(&ns));
}

void test_bb_http_section_register_ns_prefix_too_long_returns_invalid_arg(void)
{
    hs_reset();
    char prefix[BB_HTTP_SECTION_PREFIX_MAX + 2];
    memset(prefix, 'a', sizeof(prefix) - 1);
    prefix[sizeof(prefix) - 1] = '\0';
    bb_http_section_ns_t ns = { .prefix = prefix, .render = hs_render_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_section_register_ns(&ns));
}

void test_bb_http_section_register_ns_duplicate_prefix_returns_invalid_state(void)
{
    hs_reset();
    bb_http_section_ns_t a = { .prefix = "/api/hs.dup/", .render = hs_render_ok };
    bb_http_section_ns_t b = { .prefix = "/api/hs.dup/", .apply = hs_apply_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_http_section_register_ns(&a));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_http_section_register_ns(&b));
}

void test_bb_http_section_register_ns_table_full_returns_no_space(void)
{
    hs_reset();
    char prefixes[BB_HTTP_SECTION_TABLE_CAP][24];
    for (size_t i = 0; i < BB_HTTP_SECTION_TABLE_CAP; i++) {
        snprintf(prefixes[i], sizeof(prefixes[i]), "/api/hs.full%zu/", i);
        bb_http_section_ns_t ns = { .prefix = prefixes[i], .render = hs_render_ok };
        TEST_ASSERT_EQUAL(BB_OK, bb_http_section_register_ns(&ns));
    }

    bb_http_section_ns_t overflow = { .prefix = "/api/hs.overflow/", .render = hs_render_ok };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_http_section_register_ns(&overflow));
}

// ---------------------------------------------------------------------------
// bb_http_section_find
// ---------------------------------------------------------------------------

void test_bb_http_section_find_strips_prefix(void)
{
    hs_reset();
    bb_http_section_ns_t ns = { .prefix = "/api/hs.find/", .render = hs_render_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_http_section_register_ns(&ns));

    char name[BB_HTTP_SECTION_NAME_MAX];
    const bb_http_section_ns_t *found = bb_http_section_find("/api/hs.find/fan", name, sizeof(name));
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_STRING("fan", name);
}

void test_bb_http_section_find_no_match_returns_null(void)
{
    hs_reset();
    bb_http_section_ns_t ns = { .prefix = "/api/hs.nomatch/", .render = hs_render_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_http_section_register_ns(&ns));

    char name[BB_HTTP_SECTION_NAME_MAX];
    TEST_ASSERT_NULL(bb_http_section_find("/api/other/", name, sizeof(name)));
}

void test_bb_http_section_find_longest_prefix_wins(void)
{
    hs_reset();
    // Realistic (2-segment) prefixes -- "/api/" itself is rejected by the
    // minimum-segment-depth guard (see
    // test_bb_http_section_register_ns_prefix_too_broad_returns_invalid_arg
    // below), so `outer` here is the broadest ALLOWED prefix, not the
    // broadest POSSIBLE one.
    bb_http_section_ns_t outer = { .prefix = "/api/hs.lp/", .render = hs_render_ok };
    bb_http_section_ns_t inner = { .prefix = "/api/hs.lp/hs.inner/", .apply = hs_apply_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_http_section_register_ns(&outer));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_section_register_ns(&inner));

    char name[BB_HTTP_SECTION_NAME_MAX];
    const bb_http_section_ns_t *found = bb_http_section_find("/api/hs.lp/hs.inner/x", name, sizeof(name));
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_STRING("x", name);
    TEST_ASSERT_NULL(found->render);  // matched `inner`, not `outer`
    TEST_ASSERT_NOT_NULL(found->apply);
}

// Finding 3 (bb_http_section PR review, LOW): a prefix with fewer than 2
// non-empty path segments (e.g. "/api/" itself) is exactly the
// blanket-shadowing shape this dispatcher deliberately avoids -- REJECTED
// rather than convention-only.
void test_bb_http_section_register_ns_prefix_too_broad_returns_invalid_arg(void)
{
    hs_reset();
    bb_http_section_ns_t ns = { .prefix = "/api/", .render = hs_render_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_section_register_ns(&ns));
    TEST_ASSERT_EQUAL(0, bb_http_section_count());
}

void test_bb_http_section_register_ns_root_prefix_returns_invalid_arg(void)
{
    hs_reset();
    bb_http_section_ns_t ns = { .prefix = "/", .render = hs_render_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_section_register_ns(&ns));
}

// Finding 1 (bb_http_section PR review, LOW): count_path_segments() must not
// let consecutive slashes create phantom segments -- a leading double-slash
// prefix still counts as a single "api" segment (same as "/api/"), so it's
// REJECTED by the same too-broad guard, not accidentally counted as 2.
void test_bb_http_section_register_ns_double_slash_prefix_too_broad_returns_invalid_arg(void)
{
    hs_reset();
    bb_http_section_ns_t ns = { .prefix = "//api//", .render = hs_render_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_section_register_ns(&ns));
    TEST_ASSERT_EQUAL(0, bb_http_section_count());
}

// Trailing-double-slash variant: the extra slash after "api" must not be
// counted as a second (empty) segment -- still 1 segment, still rejected.
void test_bb_http_section_register_ns_trailing_double_slash_too_broad_returns_invalid_arg(void)
{
    hs_reset();
    bb_http_section_ns_t ns = { .prefix = "/api//", .render = hs_render_ok };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_http_section_register_ns(&ns));
    TEST_ASSERT_EQUAL(0, bb_http_section_count());
}

// Interior double slash between two real segments must not inflate the
// count to 3 (which would happen if an empty segment were counted) -- this
// stays a normal 2-segment prefix and is accepted.
void test_bb_http_section_register_ns_interior_double_slash_counts_as_two_segments(void)
{
    hs_reset();
    bb_http_section_ns_t ns = { .prefix = "/api//hs.section/", .render = hs_render_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_http_section_register_ns(&ns));
    TEST_ASSERT_EQUAL(1, bb_http_section_count());
}

void test_bb_http_section_find_truncates_to_out_cap(void)
{
    hs_reset();
    bb_http_section_ns_t ns = { .prefix = "/api/hs.trunc/", .render = hs_render_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_http_section_register_ns(&ns));

    char name[3];
    const bb_http_section_ns_t *found = bb_http_section_find("/api/hs.trunc/longname", name, sizeof(name));
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_STRING("lo", name);  // 3-byte cap: 2 chars + NUL
}

void test_bb_http_section_find_null_args_return_null(void)
{
    hs_reset();
    char name[BB_HTTP_SECTION_NAME_MAX];
    TEST_ASSERT_NULL(bb_http_section_find(NULL, name, sizeof(name)));
    TEST_ASSERT_NULL(bb_http_section_find("/api/x", NULL, sizeof(name)));
    TEST_ASSERT_NULL(bb_http_section_find("/api/x", name, 0));
}

// ---------------------------------------------------------------------------
// bb_http_section_count / bb_http_section_at
// ---------------------------------------------------------------------------

void test_bb_http_section_count_and_at_reflect_registrations(void)
{
    hs_reset();
    TEST_ASSERT_EQUAL(0, bb_http_section_count());

    bb_http_section_ns_t ns = { .prefix = "/api/hs.at/", .render = hs_render_ok, .apply = hs_apply_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_http_section_register_ns(&ns));
    TEST_ASSERT_EQUAL(1, bb_http_section_count());

    const char *wildcard = NULL;
    const bb_http_section_ns_t *at0 = bb_http_section_at(0, &wildcard);
    TEST_ASSERT_NOT_NULL(at0);
    TEST_ASSERT_EQUAL_STRING("/api/hs.at/*", wildcard);
    TEST_ASSERT_EQUAL_STRING("/api/hs.at/", at0->prefix);
}

void test_bb_http_section_at_out_of_range_returns_null(void)
{
    hs_reset();
    TEST_ASSERT_NULL(bb_http_section_at(0, NULL));

    bb_http_section_ns_t ns = { .prefix = "/api/hs.range/", .render = hs_render_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_http_section_register_ns(&ns));
    TEST_ASSERT_NULL(bb_http_section_at(1, NULL));
}

void test_bb_http_section_at_null_out_wildcard_is_optional(void)
{
    hs_reset();
    bb_http_section_ns_t ns = { .prefix = "/api/hs.optwc/", .render = hs_render_ok };
    TEST_ASSERT_EQUAL(BB_OK, bb_http_section_register_ns(&ns));
    TEST_ASSERT_NOT_NULL(bb_http_section_at(0, NULL));
}

// ---------------------------------------------------------------------------
// bb_http_section_status_for_render -- pure status mapping
// ---------------------------------------------------------------------------

void test_bb_http_section_status_for_render_ok_is_200(void)
{
    TEST_ASSERT_EQUAL(200, bb_http_section_status_for_render(BB_OK));
}

void test_bb_http_section_status_for_render_not_found_is_404(void)
{
    TEST_ASSERT_EQUAL(404, bb_http_section_status_for_render(BB_ERR_NOT_FOUND));
}

void test_bb_http_section_status_for_render_other_error_is_500(void)
{
    TEST_ASSERT_EQUAL(500, bb_http_section_status_for_render(BB_ERR_INVALID_STATE));
}

// ---------------------------------------------------------------------------
// bb_http_section_status_for_apply -- the by-construction stage mapping
// (B1-1022/bb_http_section PR). Full matrix per bb_http_section_status.h's
// doc, incl. the two REVERT-PROOF pins the coordinator required:
// (a) removing BB_ERR_PARSE_INCOMPLETE from the 400 mapping,
// (b) a commit-stage failure mapping to 400 instead of 500.
// ---------------------------------------------------------------------------

static bb_http_section_apply_result_t hs_result(bb_http_section_stage_t stage, bb_err_t rc)
{
    return (bb_http_section_apply_result_t){ .stage = stage, .rc = rc };
}

void test_bb_http_section_status_for_apply_parse_grammar_is_400(void)
{
    TEST_ASSERT_EQUAL(400, bb_http_section_status_for_apply(
        hs_result(BB_HTTP_SECTION_STAGE_PARSE, BB_ERR_PARSE_GRAMMAR), 0));
}

// REVERT-PROOF (a): if BB_ERR_PARSE_INCOMPLETE were dropped from the parse-
// stage 400 mapping, this test goes RED (falls through to the parse-stage
// default, 500).
void test_bb_http_section_status_for_apply_parse_incomplete_is_400(void)
{
    TEST_ASSERT_EQUAL(400, bb_http_section_status_for_apply(
        hs_result(BB_HTTP_SECTION_STAGE_PARSE, BB_ERR_PARSE_INCOMPLETE), 0));
}

void test_bb_http_section_status_for_apply_parse_not_found_is_404(void)
{
    TEST_ASSERT_EQUAL(404, bb_http_section_status_for_apply(
        hs_result(BB_HTTP_SECTION_STAGE_PARSE, BB_ERR_NOT_FOUND), 0));
}

void test_bb_http_section_status_for_apply_parse_unsupported_is_405(void)
{
    TEST_ASSERT_EQUAL(405, bb_http_section_status_for_apply(
        hs_result(BB_HTTP_SECTION_STAGE_PARSE, BB_ERR_UNSUPPORTED), 0));
}

void test_bb_http_section_status_for_apply_parse_other_error_is_500(void)
{
    TEST_ASSERT_EQUAL(500, bb_http_section_status_for_apply(
        hs_result(BB_HTTP_SECTION_STAGE_PARSE, BB_ERR_INVALID_STATE), 0));
}

void test_bb_http_section_status_for_apply_commit_ok_is_200(void)
{
    TEST_ASSERT_EQUAL(200, bb_http_section_status_for_apply(
        hs_result(BB_HTTP_SECTION_STAGE_COMMIT, BB_OK), 0));
}

void test_bb_http_section_status_for_apply_commit_validation_is_400(void)
{
    TEST_ASSERT_EQUAL(400, bb_http_section_status_for_apply(
        hs_result(BB_HTTP_SECTION_STAGE_COMMIT, BB_ERR_VALIDATION), 0));
}

void test_bb_http_section_status_for_apply_commit_unsupported_is_405(void)
{
    TEST_ASSERT_EQUAL(405, bb_http_section_status_for_apply(
        hs_result(BB_HTTP_SECTION_STAGE_COMMIT, BB_ERR_UNSUPPORTED), 0));
}

// Finding 2 (bb_http_section PR review, MEDIUM): a namespace can override
// the commit-stage BB_ERR_UNSUPPORTED status via
// bb_http_section_ns_t.unsupported_status -- 0 keeps today's 405 default
// (exercised above); a non-zero override is sent verbatim instead. 501 here
// mirrors bb_diag_http's (formerly bb_storage_http's, B1-1154) factory-reset
// route precedent (a genuine backend-capability gap, not a
// method-not-allowed shape).
void test_bb_http_section_status_for_apply_commit_unsupported_override_is_501(void)
{
    TEST_ASSERT_EQUAL(501, bb_http_section_status_for_apply(
        hs_result(BB_HTTP_SECTION_STAGE_COMMIT, BB_ERR_UNSUPPORTED), 501));
}

// A non-zero override must NOT leak into the default-405 case (BB_OK/other
// results) -- only the commit-stage BB_ERR_UNSUPPORTED branch consults it.
void test_bb_http_section_status_for_apply_override_ignored_when_not_unsupported(void)
{
    TEST_ASSERT_EQUAL(200, bb_http_section_status_for_apply(
        hs_result(BB_HTTP_SECTION_STAGE_COMMIT, BB_OK), 501));
    TEST_ASSERT_EQUAL(400, bb_http_section_status_for_apply(
        hs_result(BB_HTTP_SECTION_STAGE_COMMIT, BB_ERR_VALIDATION), 501));
}

// REVERT-PROOF (b): a genuine commit-stage failure (e.g. BB_ERR_INVALID_STATE
// -- something bb_serialize_populate()/apply() itself rejected downstream of
// a successful decode) MUST map to 500, not 400. If the commit-stage branch
// were changed to fold BB_ERR_INVALID_STATE into the same 400 bucket as
// BB_ERR_VALIDATION, this test goes RED.
void test_bb_http_section_status_for_apply_commit_other_error_is_500(void)
{
    TEST_ASSERT_EQUAL(500, bb_http_section_status_for_apply(
        hs_result(BB_HTTP_SECTION_STAGE_COMMIT, BB_ERR_INVALID_STATE), 0));
}

// ---------------------------------------------------------------------------
// End-to-end: a namespace's apply() hook driving bb_data_parse()/
// bb_data_commit() DIRECTLY against a real bb_data binding -- proves the
// helper is genuinely usable (not dead code) and that its stage tagging
// lines up with bb_data's own parse/commit split. Mirrors what a real
// bb_data-backed namespace (bb_sensors PR-2) would do; not wired to any
// actual HTTP route in this PR.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t n;
} hs_e2e_snap_t;

static const bb_serialize_field_t s_hs_e2e_fields[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(hs_e2e_snap_t, n) },
};

static const bb_serialize_desc_t s_hs_e2e_desc = {
    .type_name = "hs_e2e_snap_t", .fields = s_hs_e2e_fields, .n_fields = 1,
    .snap_size = sizeof(hs_e2e_snap_t),
};

static int64_t s_hs_e2e_applied_n = 0;

static bb_err_t hs_e2e_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    ((hs_e2e_snap_t *)dst)->n = 0;
    return BB_OK;
}

static bb_err_t hs_e2e_apply(const void *snap, const bb_data_apply_args_t *args)
{
    (void)args;
    s_hs_e2e_applied_n = ((const hs_e2e_snap_t *)snap)->n;
    return BB_OK;
}

#define HS_E2E_PARSE_SCRATCH_CAP 4096
static char s_hs_e2e_parse_scratch[HS_E2E_PARSE_SCRATCH_CAP];

// The bb_data-backed apply() hook: drives bb_data_parse() then
// bb_data_commit() directly (NOT bb_data_apply()) so it can report which
// stage its bb_err_t belongs to -- exactly the pattern this PR's design
// requires of a real bb_data-backed namespace.
static bb_http_section_apply_result_t hs_e2e_ns_apply(const char *name, const char *body,
                                                       size_t body_len, void *ctx)
{
    (void)ctx;

    bb_data_parse_req_t parse_req = {
        .fmt = BB_FORMAT_JSON, .key = name,
        .body = body, .body_len = body_len,
        .parse_scratch = s_hs_e2e_parse_scratch, .parse_scratch_cap = HS_E2E_PARSE_SCRATCH_CAP,
    };
    bb_data_parsed_t parsed;
    bb_err_t rc = bb_data_parse(&parse_req, &parsed);
    if (rc != BB_OK) {
        return (bb_http_section_apply_result_t){ .stage = BB_HTTP_SECTION_STAGE_PARSE, .rc = rc };
    }

    hs_e2e_snap_t dst;
    bb_data_commit_req_t commit_req = {
        .mode = BB_DATA_APPLY_POST, .dst_scratch = &dst, .dst_scratch_cap = sizeof(dst),
    };
    rc = bb_data_commit(&parsed, &commit_req);
    return (bb_http_section_apply_result_t){ .stage = BB_HTTP_SECTION_STAGE_COMMIT, .rc = rc };
}

void test_bb_http_section_e2e_apply_drives_bb_data_parse_and_commit(void)
{
    hs_reset();
    bb_data_test_reset();
    bb_serialize_format_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_register_format());
    s_hs_e2e_applied_n = 0;

    bb_data_binding_t binding = { .key = "hs.e2e", .desc = &s_hs_e2e_desc,
                                  .gather = hs_e2e_gather, .apply = hs_e2e_apply };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&binding));

    bb_http_section_ns_t ns = { .prefix = "/api/hs.e2e/", .apply = hs_e2e_ns_apply };
    TEST_ASSERT_EQUAL(BB_OK, bb_http_section_register_ns(&ns));

    char name[BB_HTTP_SECTION_NAME_MAX];
    const bb_http_section_ns_t *found = bb_http_section_find("/api/hs.e2e/hs.e2e", name, sizeof(name));
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_NOT_NULL(found->apply);

    const char *body = "{\"n\":9}";
    bb_http_section_apply_result_t result = found->apply(name, body, strlen(body), found->ctx);
    TEST_ASSERT_EQUAL(200, bb_http_section_status_for_apply(result, 0));
    TEST_ASSERT_EQUAL_INT64(9, s_hs_e2e_applied_n);
}

void test_bb_http_section_e2e_apply_malformed_body_maps_400(void)
{
    hs_reset();
    bb_data_test_reset();
    bb_serialize_format_test_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_register_format());
    s_hs_e2e_applied_n = -1;

    bb_data_binding_t binding = { .key = "hs.e2e.bad", .desc = &s_hs_e2e_desc,
                                  .gather = hs_e2e_gather, .apply = hs_e2e_apply };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&binding));

    bb_http_section_ns_t ns = { .prefix = "/api/hs.e2e.bad/", .apply = hs_e2e_ns_apply };
    TEST_ASSERT_EQUAL(BB_OK, bb_http_section_register_ns(&ns));

    char name[BB_HTTP_SECTION_NAME_MAX];
    const bb_http_section_ns_t *found =
        bb_http_section_find("/api/hs.e2e.bad/hs.e2e.bad", name, sizeof(name));
    TEST_ASSERT_NOT_NULL(found);

    const char *body = "{not json";
    bb_http_section_apply_result_t result = found->apply(name, body, strlen(body), found->ctx);
    TEST_ASSERT_EQUAL(400, bb_http_section_status_for_apply(result, 0));
    TEST_ASSERT_EQUAL_INT64(-1, s_hs_e2e_applied_n);  // apply() never reached
}

// test_bb_serialize_meta_openapi_topic_schema — Unity coverage for
// bb_serialize_meta_openapi_topic_schema()/bb_serialize_meta_ensure_topic_
// schema() (B1-1059 SSE batch PR-1) in THIS env's own compiled variant (no
// BB_SERIALIZE_META_TESTING here -- see test_bb_serialize_meta_openapi_test_
// seam.c's own comment for why the same source line's branch graph differs
// per env; the force-no-space seam arms live only in
// test/test_host_openapi_meta_on/). Reuses the same synthetic widget
// fixture (bb_fixture_widget_desc/_meta) as the golden test above.

#include "unity.h"

#include "bb_serialize_meta.h"

#include "test_serialize_fixture.h"

#include <stdio.h>
#include <string.h>

extern const bb_serialize_desc_meta_t bb_fixture_widget_meta;

static const char *const s_title = "Widget";
static const char *const s_topic = "widgets";

void test_bb_serialize_meta_openapi_topic_schema_composes_exact_prefix_and_body(void)
{
    char   body[1536];
    size_t body_n = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_fixture_widget_desc, &bb_fixture_widget_meta, body, sizeof body, &body_n));

    char expected[1600];
    snprintf(expected, sizeof(expected), "{\"title\":\"Widget\",\"x-sse-topic\":\"widgets\",%s", body + 1);

    char   out[1600];
    size_t n = 0;
    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                            &bb_fixture_widget_desc, &bb_fixture_widget_meta,
                                                            s_title, s_topic, out, sizeof out, &n);

    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING(expected, out);
    TEST_ASSERT_EQUAL_UINT(strlen(expected), n);
}

void test_bb_serialize_meta_openapi_topic_schema_render_cap_zero(void)
{
    char out[4] = { 'x', 'x', 'x', 'x' };
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE,
        bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                &bb_fixture_widget_desc, &bb_fixture_widget_meta,
                                                s_title, s_topic, out, 0, NULL));
}

// Buffer too small to hold the `{"title":..,"x-sse-topic":..,` prefix --
// `composer` is never reached (ctx.err != BB_OK before composer() is
// called).
void test_bb_serialize_meta_openapi_topic_schema_prefix_overflow_too_small_cap(void)
{
    char   out[10];
    size_t n = 12345;

    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                            &bb_fixture_widget_desc, &bb_fixture_widget_meta,
                                                            s_title, s_topic, out, sizeof out, &n);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_UINT(0, n);
}

void test_bb_serialize_meta_openapi_topic_schema_prefix_overflow_null_out_len(void)
{
    char out[10];

    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                            &bb_fixture_widget_desc, &bb_fixture_widget_meta,
                                                            s_title, s_topic, out, sizeof out, NULL);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", out);
}

// Buffer big enough for the prefix but too small for `composer`'s own body
// -- exercises the `rc != BB_OK` branch after the prefix write succeeds.
void test_bb_serialize_meta_openapi_topic_schema_composer_overflow_too_small_cap(void)
{
    // 100 bytes clears the ~44-byte `{"title":..,"x-sse-topic":..,` prefix
    // (so the overflow genuinely happens inside `composer`, not the prefix
    // write) but is nowhere near the widget fixture's full composed body
    // (~1.1KB, see test_bb_serialize_meta_openapi_widget_golden's golden
    // string).
    char   out[100];
    size_t n = 12345;

    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                            &bb_fixture_widget_desc, &bb_fixture_widget_meta,
                                                            s_title, s_topic, out, sizeof out, &n);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_UINT(0, n);
}

void test_bb_serialize_meta_openapi_topic_schema_composer_overflow_null_out_len(void)
{
    char out[100];

    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                            &bb_fixture_widget_desc, &bb_fixture_widget_meta,
                                                            s_title, s_topic, out, sizeof out, NULL);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_bb_serialize_meta_openapi_topic_schema_success_null_out_len(void)
{
    char out[1600];

    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                            &bb_fixture_widget_desc, &bb_fixture_widget_meta,
                                                            s_title, s_topic, out, sizeof out, NULL);

    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
}

// ---------------------------------------------------------------------------
// Mock bb_serialize_meta_composer_fn implementations -- exercise
// bb_serialize_meta_openapi_topic_schema()'s defensive
// `body_len == 0 || out[prefix_len] != '{'` guard, which a REAL composer
// (bb_serialize_meta_openapi_schema()/_fragment(), always `{`-leading and
// non-empty on success) can never trip. `desc`/`meta` are unused. THIS
// env's own compiled variant needs its own coverage of the guard, same
// reasoning as every other branch in this file (see the file banner).
// ---------------------------------------------------------------------------

// Reports success with a zero-length body -- covers the `body_len == 0` arm.
static bb_err_t mock_composer_empty_success(const bb_serialize_desc_t      *desc,
                                             const bb_serialize_desc_meta_t *meta,
                                             char *out, size_t out_size, size_t *out_len)
{
    (void)desc;
    (void)meta;
    if (out_size == 0) return BB_ERR_NO_SPACE;
    out[0] = '\0';
    if (out_len) *out_len = 0;
    return BB_OK;
}

// Reports success with a non-empty body that does NOT start with '{' --
// covers the `out[prefix_len] != '{'` arm.
static bb_err_t mock_composer_no_leading_brace(const bb_serialize_desc_t      *desc,
                                                const bb_serialize_desc_meta_t *meta,
                                                char *out, size_t out_size, size_t *out_len)
{
    (void)desc;
    (void)meta;
    static const char body[] = ",\"type\":\"object\"}";
    if (out_size < sizeof(body)) return BB_ERR_NO_SPACE;
    memcpy(out, body, sizeof(body));
    if (out_len) *out_len = sizeof(body) - 1;
    return BB_OK;
}

void test_bb_serialize_meta_openapi_topic_schema_rejects_zero_len_composer_body(void)
{
    char   out[64];
    size_t n = 12345;

    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(mock_composer_empty_success,
                                                            &bb_fixture_widget_desc, &bb_fixture_widget_meta,
                                                            s_title, s_topic, out, sizeof out, &n);

    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, rc);
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_UINT(0, n);
}

void test_bb_serialize_meta_openapi_topic_schema_rejects_non_brace_composer_body_null_out_len(void)
{
    char out[64];

    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(mock_composer_no_leading_brace,
                                                            &bb_fixture_widget_desc, &bb_fixture_widget_meta,
                                                            s_title, s_topic, out, sizeof out, NULL);

    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, rc);
    TEST_ASSERT_EQUAL_STRING("", out);
}

// ---------------------------------------------------------------------------
// bb_serialize_meta_ensure_topic_schema() -- idempotent-sentinel wrapper,
// THIS env's own compiled variant.
// ---------------------------------------------------------------------------

void test_bb_serialize_meta_ensure_topic_schema_composes_when_buf_empty(void)
{
    char buf[1600] = { 0 };

    bb_err_t rc = bb_serialize_meta_ensure_topic_schema(bb_serialize_meta_openapi_schema,
                                                          &bb_fixture_widget_desc, &bb_fixture_widget_meta,
                                                          s_title, s_topic, buf, sizeof buf);

    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_NOT_EQUAL(0, buf[0]);
}

void test_bb_serialize_meta_ensure_topic_schema_overflow_leaves_buf_empty(void)
{
    char buf[10] = { 0 };

    bb_err_t rc = bb_serialize_meta_ensure_topic_schema(bb_serialize_meta_openapi_schema,
                                                          &bb_fixture_widget_desc, &bb_fixture_widget_meta,
                                                          s_title, s_topic, buf, sizeof buf);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL(0, buf[0]);
}

// Idempotent no-op: a second call against an already-composed buf never
// re-invokes bb_serialize_meta_openapi_topic_schema() -- proven here by
// pointer-stable content across two calls (no fail-injection seam available
// in this env; test/test_host_openapi_meta_on's sibling test proves the
// stronger force-no-space-on-2nd-call variant).
void test_bb_serialize_meta_ensure_topic_schema_idempotent_same_content(void)
{
    char buf[1600] = { 0 };

    bb_err_t first = bb_serialize_meta_ensure_topic_schema(bb_serialize_meta_openapi_schema,
                                                             &bb_fixture_widget_desc, &bb_fixture_widget_meta,
                                                             s_title, s_topic, buf, sizeof buf);
    TEST_ASSERT_EQUAL_INT(BB_OK, first);
    char snapshot[1600];
    strcpy(snapshot, buf);

    bb_err_t second = bb_serialize_meta_ensure_topic_schema(bb_serialize_meta_openapi_schema,
                                                              &bb_fixture_widget_desc, &bb_fixture_widget_meta,
                                                              s_title, s_topic, buf, sizeof buf);

    TEST_ASSERT_EQUAL_INT(BB_OK, second);
    TEST_ASSERT_EQUAL_STRING(snapshot, buf);
}

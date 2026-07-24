#include "unity.h"
#include "bb_mqtt_client.h"
#include "bb_serialize_meta.h"
#include "bb_serialize_meta_test.h"

#include <stdio.h>
#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// engine-level coverage for bb_serialize_meta_openapi_topic_schema() /
// bb_serialize_meta_ensure_topic_schema() (B1-1059 SSE batch PR-1): the
// top-level `{"title":..,"x-sse-topic":..,` prefix splice ahead of the SAME
// body a composer (typically bb_serialize_meta_openapi_schema()) produces.
// ZERO real consumers this PR -- the 5 SSE topic route sites adopt this in
// a later PR -- so every arm is exercised directly here rather than via a
// per-component wiring test. Reuses the same real desc/meta fixture the
// sibling ensure_composed suite relies on
// (bb_mqtt_client_health_section_desc/_meta, from bb_mqtt_client.h).

static const char *const s_title = "T";
static const char *const s_topic = "S";

// Composes the SAME body bb_serialize_meta_openapi_schema() would, so tests
// can build an exact expected string without hand-duplicating the schema
// literal (which would drift if the fixture's desc/meta ever changes).
static void compose_expected_body(char *body, size_t body_size)
{
    size_t n = 0;
    bb_err_t rc = bb_serialize_meta_openapi_schema(&bb_mqtt_client_health_section_desc,
                                                     &bb_mqtt_client_health_section_meta,
                                                     body, body_size, &n);
    TEST_ASSERT_EQUAL(BB_OK, rc);
}

// ---------------------------------------------------------------------------
// Mock bb_serialize_meta_composer_fn implementations -- exercise
// bb_serialize_meta_openapi_topic_schema()'s defensive
// `body_len == 0 || out[prefix_len] != '{'` guard, which a REAL composer
// (bb_serialize_meta_openapi_schema()/_fragment(), always `{`-leading and
// non-empty on success) can never trip. `desc`/`meta` are unused -- mirrors
// the seam tests' NULL-desc/meta convention (test_bb_serialize_meta_openapi_
// test_seam.c) since these mocks never dereference them.
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
// covers the `out[prefix_len] != '{'` arm (e.g. an open_fragment()-shaped
// composer, whose body never closes with a leading standalone object).
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
                                                            &bb_mqtt_client_health_section_desc,
                                                            &bb_mqtt_client_health_section_meta,
                                                            s_title, s_topic, out, sizeof(out), &n);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL(0, n);
}

void test_bb_serialize_meta_openapi_topic_schema_rejects_non_brace_composer_body_null_out_len(void)
{
    char out[64];

    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(mock_composer_no_leading_brace,
                                                            &bb_mqtt_client_health_section_desc,
                                                            &bb_mqtt_client_health_section_meta,
                                                            s_title, s_topic, out, sizeof(out), NULL);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
    TEST_ASSERT_EQUAL_STRING("", out);
}

// Title/topic containing '"' and '\\' -- proves the prefix write escapes
// them correctly (bb_oa_put_qstr(), the same helper every field value in
// this engine goes through) rather than emitting broken JSON.
void test_bb_serialize_meta_openapi_topic_schema_escapes_title_and_topic(void)
{
    static const char *const tricky_title = "Say \"Hi\"\\Now";
    static const char *const tricky_topic = "a\"b\\c";

    char body[512];
    compose_expected_body(body, sizeof(body));

    char expected[600];
    snprintf(expected, sizeof(expected),
              "{\"title\":\"Say \\\"Hi\\\"\\\\Now\",\"x-sse-topic\":\"a\\\"b\\\\c\",%s", body + 1);

    char   out[600];
    size_t n = 0;
    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                            &bb_mqtt_client_health_section_desc,
                                                            &bb_mqtt_client_health_section_meta,
                                                            tricky_title, tricky_topic, out, sizeof(out), &n);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING(expected, out);
    TEST_ASSERT_EQUAL_UINT(strlen(expected), n);
}

// ---------------------------------------------------------------------------
// bb_serialize_meta_openapi_topic_schema() -- direct calls
// ---------------------------------------------------------------------------

void test_bb_serialize_meta_openapi_topic_schema_composes_exact_prefix_and_body(void)
{
    char body[512];
    compose_expected_body(body, sizeof(body));

    char expected[600];
    snprintf(expected, sizeof(expected), "{\"title\":\"T\",\"x-sse-topic\":\"S\",%s", body + 1);

    char   out[600];
    size_t n = 0;
    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                            &bb_mqtt_client_health_section_desc,
                                                            &bb_mqtt_client_health_section_meta,
                                                            s_title, s_topic, out, sizeof(out), &n);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING(expected, out);
    TEST_ASSERT_EQUAL_UINT(strlen(expected), n);
}

void test_bb_serialize_meta_openapi_topic_schema_force_no_space(void)
{
    char buf[64] = "unwritten";
    size_t len = 99;

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                            &bb_mqtt_client_health_section_desc,
                                                            &bb_mqtt_client_health_section_meta,
                                                            s_title, s_topic, buf, sizeof(buf), &len);
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", buf);
    TEST_ASSERT_EQUAL(0, len);
}

void test_bb_serialize_meta_openapi_topic_schema_force_no_space_null_out_len(void)
{
    char buf[64] = "unwritten";

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                            &bb_mqtt_client_health_section_desc,
                                                            &bb_mqtt_client_health_section_meta,
                                                            s_title, s_topic, buf, sizeof(buf), NULL);
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

// Buffer too small to even hold the `{"title":..,"x-sse-topic":..,` prefix
// -- `composer` is never reached; the overflow happens inside the prefix
// write itself (ctx.err != BB_OK before composer() is called).
void test_bb_serialize_meta_openapi_topic_schema_prefix_overflow_leaves_buf_empty(void)
{
    char   buf[10];
    size_t n = 12345;

    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                            &bb_mqtt_client_health_section_desc,
                                                            &bb_mqtt_client_health_section_meta,
                                                            s_title, s_topic, buf, sizeof(buf), &n);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", buf);
    TEST_ASSERT_EQUAL(0, n);
}

void test_bb_serialize_meta_openapi_topic_schema_prefix_overflow_null_out_len(void)
{
    char buf[10];

    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                            &bb_mqtt_client_health_section_desc,
                                                            &bb_mqtt_client_health_section_meta,
                                                            s_title, s_topic, buf, sizeof(buf), NULL);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

// Buffer big enough for the prefix but too small for `composer`'s own body
// -- exercises the `rc != BB_OK` branch after the prefix write succeeds.
void test_bb_serialize_meta_openapi_topic_schema_composer_overflow_leaves_buf_empty(void)
{
    char   buf[40];
    size_t n = 12345;

    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                            &bb_mqtt_client_health_section_desc,
                                                            &bb_mqtt_client_health_section_meta,
                                                            s_title, s_topic, buf, sizeof(buf), &n);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", buf);
    TEST_ASSERT_EQUAL(0, n);
}

void test_bb_serialize_meta_openapi_topic_schema_composer_overflow_null_out_len(void)
{
    char buf[40];

    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                            &bb_mqtt_client_health_section_desc,
                                                            &bb_mqtt_client_health_section_meta,
                                                            s_title, s_topic, buf, sizeof(buf), NULL);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_bb_serialize_meta_openapi_topic_schema_success_null_out_len(void)
{
    char out[600];

    bb_err_t rc = bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                            &bb_mqtt_client_health_section_desc,
                                                            &bb_mqtt_client_health_section_meta,
                                                            s_title, s_topic, out, sizeof(out), NULL);

    TEST_ASSERT_EQUAL(BB_OK, rc);
}

// ---------------------------------------------------------------------------
// bb_serialize_meta_ensure_topic_schema() -- idempotent-sentinel wrapper
// ---------------------------------------------------------------------------

void test_bb_serialize_meta_ensure_topic_schema_composes_when_buf_empty(void)
{
    char buf[600] = { 0 };

    bb_err_t rc = bb_serialize_meta_ensure_topic_schema(bb_serialize_meta_openapi_schema,
                                                          &bb_mqtt_client_health_section_desc,
                                                          &bb_mqtt_client_health_section_meta,
                                                          s_title, s_topic, buf, sizeof(buf));

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_NOT_EQUAL(0, buf[0]);
}

void test_bb_serialize_meta_ensure_topic_schema_overflow_leaves_buf_empty(void)
{
    char buf[10] = { 0 };

    bb_err_t rc = bb_serialize_meta_ensure_topic_schema(bb_serialize_meta_openapi_schema,
                                                          &bb_mqtt_client_health_section_desc,
                                                          &bb_mqtt_client_health_section_meta,
                                                          s_title, s_topic, buf, sizeof(buf));

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL(0, buf[0]);
}

// Proves the idempotency sentinel short-circuits BEFORE bb_serialize_meta_
// openapi_topic_schema() is ever (re-)invoked: force the engine-level
// fail-injection seam to BB_ERR_NO_SPACE only on the SECOND call (after a
// real, successful first compose) -- if the sentinel didn't short-circuit,
// this second call would itself return BB_ERR_NO_SPACE.
void test_bb_serialize_meta_ensure_topic_schema_idempotent_second_call_is_noop(void)
{
    char buf[600] = { 0 };

    bb_err_t first = bb_serialize_meta_ensure_topic_schema(bb_serialize_meta_openapi_schema,
                                                             &bb_mqtt_client_health_section_desc,
                                                             &bb_mqtt_client_health_section_meta,
                                                             s_title, s_topic, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(BB_OK, first);
    char snapshot[600];
    strcpy(snapshot, buf);

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    bb_err_t second = bb_serialize_meta_ensure_topic_schema(bb_serialize_meta_openapi_schema,
                                                              &bb_mqtt_client_health_section_desc,
                                                              &bb_mqtt_client_health_section_meta,
                                                              s_title, s_topic, buf, sizeof(buf));
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_EQUAL(BB_OK, second);
    TEST_ASSERT_EQUAL_STRING(snapshot, buf);
}

// Dispatch works for bb_serialize_meta_openapi_schema() passed as composer
// -- output matches calling bb_serialize_meta_openapi_topic_schema()
// directly.
void test_bb_serialize_meta_ensure_topic_schema_dispatches_schema_composer(void)
{
    char buf[600]    = { 0 };
    char direct[600];
    size_t n = 0;

    bb_err_t rc = bb_serialize_meta_ensure_topic_schema(bb_serialize_meta_openapi_schema,
                                                          &bb_mqtt_client_health_section_desc,
                                                          &bb_mqtt_client_health_section_meta,
                                                          s_title, s_topic, buf, sizeof(buf));
    bb_err_t direct_rc = bb_serialize_meta_openapi_topic_schema(bb_serialize_meta_openapi_schema,
                                                                   &bb_mqtt_client_health_section_desc,
                                                                   &bb_mqtt_client_health_section_meta,
                                                                   s_title, s_topic, direct, sizeof(direct), &n);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(BB_OK, direct_rc);
    TEST_ASSERT_EQUAL_STRING(direct, buf);
}

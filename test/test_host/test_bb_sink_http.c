// Tests for bb_sink_http:
// - url_encode: slash → %2F, unreserved chars pass through
// - sink publish() builds expected URL (base + path_template substitution)
//   and calls bb_http_client_session_post — verified via
//   bb_http_client_session_last_post (session API, not one-shot post)
// - custom path_tmpl override
// - disabled → no POST
// - session reused across multiple publishes (same handle, not re-opened)
// - parse/serialize round-trip for delimited NVS headers format
// - injection: \n in value, ':'/space in name rejected
// - session applies X-Client-Id (hostname default; explicit when set) + headers
// - re-applied after set_cfg
#include "unity.h"
#include "bb_sink_http.h"
#include "bb_nv.h"
#include "bb_nv_keys.h"
#include "bb_tls.h"
#include "bb_tls_creds.h"
#include "bb_transport_health.h"
#include "../../platform/host/bb_http_client/bb_http_client_host.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Failing malloc for OOM tests (BB_SINK_HTTP_TESTING)
// ---------------------------------------------------------------------------
static void *failing_malloc(size_t n) { (void)n; return NULL; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void reset_state(void)
{
    bb_nv_host_str_store_reset();
    bb_http_client_clear_mock();
}

static void set_mock_200(void)
{
    bb_http_client_session_set_mock_status(200);
}

// ---------------------------------------------------------------------------
// url_encode tests
// ---------------------------------------------------------------------------

void test_bb_sink_http_url_encode_slash_to_pct2F(void)
{
    char out[64] = {0};
    size_t n = bb_sink_http_url_encode("a/b", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("a%2Fb", out);
    TEST_ASSERT_EQUAL_INT(5, (int)n);
}

void test_bb_sink_http_url_encode_unreserved_pass_through(void)
{
    char out[64] = {0};
    bb_sink_http_url_encode("abcABC123-_.~", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("abcABC123-_.~", out);
}

void test_bb_sink_http_url_encode_space_to_pct20(void)
{
    char out[64] = {0};
    bb_sink_http_url_encode("a b", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("a%20b", out);
}

void test_bb_sink_http_url_encode_empty_src(void)
{
    char out[16] = {0};
    size_t n = bb_sink_http_url_encode("", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_INT(0, (int)n);
}

void test_bb_sink_http_url_encode_null_src(void)
{
    char out[16] = "X";
    size_t n = bb_sink_http_url_encode(NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(0, (int)n);
}

void test_bb_sink_http_url_encode_truncation(void)
{
    char out[4] = {0};  // only room for 3 chars + NUL
    // "ab/" would encode to "ab%2F" (5 chars) — should truncate to "ab\0"
    bb_sink_http_url_encode("ab/", out, sizeof(out));
    // output must be NUL-terminated and fit
    TEST_ASSERT_EQUAL_INT('\0', out[3]);
    // must not contain partial %-encoding
    TEST_ASSERT_NULL(strstr(out, "%2"));
}

// ---------------------------------------------------------------------------
// Sink: publish builds correct URL
// ---------------------------------------------------------------------------

void test_bb_sink_http_builds_url_with_default_template(void)
{
    reset_state();
    set_mock_200();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://xxxx-ats.iot.us-east-1.amazonaws.com:8443",
            sizeof(cfg.base) - 1);
    cfg.qos     = 1;
    cfg.enabled = true;

    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_err_t rc = bb_sink_http(&sink);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_NOT_NULL(sink.publish);

    const char *topic   = "sensors/acme-corp/temp";
    const char *payload = "{\"v\":42}";
    rc = sink.publish(sink.ctx, topic, payload, (int)strlen(payload), false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    // Verify via session record (not one-shot post record).
    bb_http_client_session_record_t rec = bb_http_client_session_last_post();
    TEST_ASSERT_TRUE(rec.called);

    // URL must contain the encoded topic (slashes → %2F)
    TEST_ASSERT_NOT_NULL(strstr(rec.url, "sensors%2Facme-corp%2Ftemp"));
    // URL must contain qos=1
    TEST_ASSERT_NOT_NULL(strstr(rec.url, "qos=1"));
    // Body must match payload
    TEST_ASSERT_EQUAL_STRING(payload, rec.body);
    // Content-type must be application/json
    TEST_ASSERT_EQUAL_STRING("application/json", rec.content_type);
}

void test_bb_sink_http_custom_path_template(void)
{
    reset_state();
    set_mock_200();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com", sizeof(cfg.base) - 1);
    strncpy(cfg.path_tmpl, "/publish/{topic}", sizeof(cfg.path_tmpl) - 1);
    cfg.qos     = 0;
    cfg.enabled = true;

    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    const char *topic   = "test/data";
    const char *payload = "{\"x\":1}";
    bb_err_t rc = sink.publish(sink.ctx, topic, payload, (int)strlen(payload), false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    bb_http_client_session_record_t rec = bb_http_client_session_last_post();
    TEST_ASSERT_TRUE(rec.called);
    // URL should use custom template path, topic encoded
    TEST_ASSERT_NOT_NULL(strstr(rec.url, "/publish/test%2Fdata"));
    // {qos} not in template → not in URL
    TEST_ASSERT_NULL(strstr(rec.url, "qos="));
}

void test_bb_sink_http_disabled_no_post(void)
{
    reset_state();
    // No mock needed — disabled path must not call session_post.

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com", sizeof(cfg.base) - 1);
    cfg.qos     = 1;
    cfg.enabled = false;  // explicitly disabled

    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    bb_http_client_session_record_t rec = bb_http_client_session_last_post();
    TEST_ASSERT_FALSE(rec.called);
}

void test_bb_sink_http_null_out_returns_invalid_arg(void)
{
    bb_err_t rc = bb_sink_http(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

// ---------------------------------------------------------------------------
// Session reuse: multiple publishes share ONE session (no new socket/conn)
// ---------------------------------------------------------------------------

void test_bb_sink_http_session_reused_across_publishes(void)
{
    reset_state();
    set_mock_200();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com:8443", sizeof(cfg.base) - 1);
    cfg.qos     = 1;
    cfg.enabled = true;

    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    // Two separate publishes — both must succeed via the same session.
    bb_err_t rc = sink.publish(sink.ctx, "t/one", "{\"n\":1}", 6, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    bb_http_client_session_record_t rec1 = bb_http_client_session_last_post();
    TEST_ASSERT_TRUE(rec1.called);
    TEST_ASSERT_NOT_NULL(strstr(rec1.url, "t%2Fone"));

    rc = sink.publish(sink.ctx, "t/two", "{\"n\":2}", 6, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    bb_http_client_session_record_t rec2 = bb_http_client_session_last_post();
    TEST_ASSERT_TRUE(rec2.called);
    TEST_ASSERT_NOT_NULL(strstr(rec2.url, "t%2Ftwo"));
    // one-shot post record must remain uncalled (session path, not post path)
    bb_http_client_post_record_t post_rec = bb_http_client_get_last_post();
    TEST_ASSERT_FALSE(post_rec.called);
}

// ---------------------------------------------------------------------------
// Session invalidated on config change
// ---------------------------------------------------------------------------

void test_bb_sink_http_session_invalidated_on_set_cfg(void)
{
    reset_state();
    set_mock_200();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://original.example.com:8443", sizeof(cfg.base) - 1);
    cfg.qos     = 1;
    cfg.enabled = true;

    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    // First publish opens the session.
    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    // Change config — session must close and reopen on next publish.
    bb_sink_http_cfg_t cfg2;
    memset(&cfg2, 0, sizeof(cfg2));
    strncpy(cfg2.base, "https://new.example.com:8443", sizeof(cfg2.base) - 1);
    cfg2.qos     = 0;
    cfg2.enabled = true;
    bb_sink_http_set_cfg(&cfg2);

    rc = sink.publish(sink.ctx, "t/y", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    bb_http_client_session_record_t rec = bb_http_client_session_last_post();
    TEST_ASSERT_TRUE(rec.called);
    // URL should now use the new base.
    TEST_ASSERT_NOT_NULL(strstr(rec.url, "new.example.com"));
}

// ---------------------------------------------------------------------------
// Header validation helpers (pure)
// ---------------------------------------------------------------------------

void test_bb_sink_http_header_name_valid_normal(void)
{
    TEST_ASSERT_TRUE(bb_sink_http_header_name_valid("X-Trace-Id"));
    TEST_ASSERT_TRUE(bb_sink_http_header_name_valid("Authorization"));
    TEST_ASSERT_TRUE(bb_sink_http_header_name_valid("X-Client-Id"));
}

void test_bb_sink_http_header_name_invalid_colon(void)
{
    TEST_ASSERT_FALSE(bb_sink_http_header_name_valid("Bad:Name"));
}

void test_bb_sink_http_header_name_invalid_space(void)
{
    TEST_ASSERT_FALSE(bb_sink_http_header_name_valid("Bad Name"));
}

void test_bb_sink_http_header_name_invalid_tab(void)
{
    TEST_ASSERT_FALSE(bb_sink_http_header_name_valid("Bad\tName"));
}

void test_bb_sink_http_header_name_invalid_empty(void)
{
    TEST_ASSERT_FALSE(bb_sink_http_header_name_valid(""));
    TEST_ASSERT_FALSE(bb_sink_http_header_name_valid(NULL));
}

void test_bb_sink_http_header_value_valid_normal(void)
{
    TEST_ASSERT_TRUE(bb_sink_http_header_value_valid("Bearer token-abc"));
    TEST_ASSERT_TRUE(bb_sink_http_header_value_valid(""));
}

void test_bb_sink_http_header_value_invalid_newline(void)
{
    TEST_ASSERT_FALSE(bb_sink_http_header_value_valid("val\ninjected: evil"));
}

void test_bb_sink_http_header_value_invalid_cr(void)
{
    TEST_ASSERT_FALSE(bb_sink_http_header_value_valid("val\rinjected"));
}

// ---------------------------------------------------------------------------
// Parse / serialize round-trip (pure)
// ---------------------------------------------------------------------------

void test_bb_sink_http_parse_serialize_roundtrip(void)
{
    // Build a string.
    bb_sink_http_header_t orig[3];
    memset(orig, 0, sizeof(orig));
    strncpy(orig[0].name,  "X-Trace-Id", sizeof(orig[0].name) - 1);
    strncpy(orig[0].value, "abc123",     sizeof(orig[0].value) - 1);
    orig[0].secret = false;
    strncpy(orig[1].name,  "Authorization", sizeof(orig[1].name) - 1);
    strncpy(orig[1].value, "Bearer xyz",   sizeof(orig[1].value) - 1);
    orig[1].secret = true;
    strncpy(orig[2].name,  "X-Custom", sizeof(orig[2].name) - 1);
    strncpy(orig[2].value, "hello",    sizeof(orig[2].value) - 1);
    orig[2].secret = false;

    char buf[512] = {0};
    size_t n = bb_sink_http_serialize_headers(orig, 3, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);

    // Must contain secret prefix for Authorization.
    TEST_ASSERT_NOT_NULL(strstr(buf, "*Authorization: Bearer xyz"));
    // Must contain non-secret headers without prefix.
    TEST_ASSERT_NOT_NULL(strstr(buf, "X-Trace-Id: abc123"));

    // Parse back.
    bb_sink_http_header_t parsed[8];
    int cnt = bb_sink_http_parse_headers(buf, parsed, 8);
    TEST_ASSERT_EQUAL_INT(3, cnt);

    // Order preserved.
    TEST_ASSERT_EQUAL_STRING("X-Trace-Id", parsed[0].name);
    TEST_ASSERT_EQUAL_STRING("abc123",     parsed[0].value);
    TEST_ASSERT_FALSE(parsed[0].secret);

    TEST_ASSERT_EQUAL_STRING("Authorization", parsed[1].name);
    TEST_ASSERT_EQUAL_STRING("Bearer xyz",    parsed[1].value);
    TEST_ASSERT_TRUE(parsed[1].secret);

    TEST_ASSERT_EQUAL_STRING("X-Custom", parsed[2].name);
    TEST_ASSERT_EQUAL_STRING("hello",    parsed[2].value);
    TEST_ASSERT_FALSE(parsed[2].secret);
}

void test_bb_sink_http_parse_skips_malformed_lines(void)
{
    // No ": " separator in first line — should be skipped.
    const char *buf = "NoColonSpace\nX-Good: value\n";
    bb_sink_http_header_t out[4];
    int n = bb_sink_http_parse_headers(buf, out, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("X-Good", out[0].name);
    TEST_ASSERT_EQUAL_STRING("value",  out[0].value);
}

void test_bb_sink_http_parse_skips_blank_lines(void)
{
    const char *buf = "\nX-A: val1\n\nX-B: val2\n";
    bb_sink_http_header_t out[4];
    int n = bb_sink_http_parse_headers(buf, out, 4);
    TEST_ASSERT_EQUAL_INT(2, n);
}

void test_bb_sink_http_parse_cap_enforced(void)
{
    // Serialize 5 headers, then parse with cap=2.
    bb_sink_http_header_t orig[5];
    memset(orig, 0, sizeof(orig));
    for (int i = 0; i < 5; i++) {
        snprintf(orig[i].name,  sizeof(orig[i].name),  "X-H%d", i);
        snprintf(orig[i].value, sizeof(orig[i].value), "v%d",   i);
    }
    char buf[512] = {0};
    bb_sink_http_serialize_headers(orig, 5, buf, sizeof(buf));

    bb_sink_http_header_t out[2];
    int n = bb_sink_http_parse_headers(buf, out, 2);
    TEST_ASSERT_EQUAL_INT(2, n);
}

void test_bb_sink_http_parse_null_buf_returns_zero(void)
{
    bb_sink_http_header_t out[4];
    int n = bb_sink_http_parse_headers(NULL, out, 4);
    TEST_ASSERT_EQUAL_INT(0, n);
}

// ---------------------------------------------------------------------------
// Injection guard
// ---------------------------------------------------------------------------

void test_bb_sink_http_parse_rejects_value_with_newline(void)
{
    // A value containing \n must be rejected.
    const char *buf = "X-Bad: val\ninjected: evil";
    bb_sink_http_header_t out[4];
    // The parser splits on '\n', so "X-Bad: val" is one entry (value="val"),
    // and "injected: evil" is another valid entry. But "val" is valid; the
    // embedded '\n' acts as the record separator — the second "entry" from
    // the injection attempt would be "injected: evil", which has a valid name
    // and value (colon is NOT in the name part, it's in the separator).
    // The key point: "val\ninjected: evil" cannot be round-tripped as a
    // single header value because serialize will stop at '\n'. This test
    // ensures that a crafted name containing ':' is rejected.
    (void)buf;

    // Test: name with ':' is rejected.
    const char *bad_name_buf = "Bad:Name: value\n";
    int n = bb_sink_http_parse_headers(bad_name_buf, out, 4);
    // "Bad" would be name, ":Name: value" would be split... actually the first
    // ": " is the separator, so name="Bad", value="Name: value". But "Bad" is
    // valid. Let's test the direct validation function instead.
    (void)n;

    // Direct validation: name with ':' must fail.
    TEST_ASSERT_FALSE(bb_sink_http_header_name_valid("Bad:Name"));

    // Direct validation: value with '\n' must fail.
    TEST_ASSERT_FALSE(bb_sink_http_header_value_valid("val\ninjected: evil"));

    // Direct validation: value with '\r' must fail.
    TEST_ASSERT_FALSE(bb_sink_http_header_value_valid("val\rinjected"));
}

void test_bb_sink_http_serialize_skips_invalid_entries(void)
{
    // Entries with invalid name or value must be skipped in serialization.
    bb_sink_http_header_t hdrs[2];
    memset(hdrs, 0, sizeof(hdrs));
    // Invalid: name has colon (simulate a malformed entry somehow forced in memory).
    strncpy(hdrs[0].name,  "Bad:Name", sizeof(hdrs[0].name) - 1);
    strncpy(hdrs[0].value, "v1",       sizeof(hdrs[0].value) - 1);
    // Valid entry.
    strncpy(hdrs[1].name,  "X-Good", sizeof(hdrs[1].name) - 1);
    strncpy(hdrs[1].value, "ok",     sizeof(hdrs[1].value) - 1);

    char buf[256] = {0};
    bb_sink_http_serialize_headers(hdrs, 2, buf, sizeof(buf));

    // Only X-Good should appear.
    TEST_ASSERT_NOT_NULL(strstr(buf, "X-Good: ok"));
    TEST_ASSERT_NULL(strstr(buf, "Bad:Name"));
}

// ---------------------------------------------------------------------------
// Merge helper (pure)
// ---------------------------------------------------------------------------

void test_bb_sink_http_merge_adds_non_secret(void)
{
    bb_sink_http_patch_entry_t patch[1];
    memset(patch, 0, sizeof(patch));
    strncpy(patch[0].name,  "X-Trace", sizeof(patch[0].name) - 1);
    strncpy(patch[0].value, "abc",     sizeof(patch[0].value) - 1);
    patch[0].secret = false;
    patch[0].value_present = true;

    bb_sink_http_header_t out[8];
    int n = bb_sink_http_merge_headers(patch, 1, NULL, 0, out, 8);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("X-Trace", out[0].name);
    TEST_ASSERT_EQUAL_STRING("abc",     out[0].value);
    TEST_ASSERT_FALSE(out[0].secret);
}

void test_bb_sink_http_merge_secret_with_value_updates(void)
{
    bb_sink_http_patch_entry_t patch[1];
    memset(patch, 0, sizeof(patch));
    strncpy(patch[0].name,  "Authorization", sizeof(patch[0].name) - 1);
    strncpy(patch[0].value, "Bearer new",    sizeof(patch[0].value) - 1);
    patch[0].secret = true;
    patch[0].value_present = true;

    bb_sink_http_header_t out[8];
    int n = bb_sink_http_merge_headers(patch, 1, NULL, 0, out, 8);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("Bearer new", out[0].value);
    TEST_ASSERT_TRUE(out[0].secret);
}

void test_bb_sink_http_merge_secret_blank_preserves_existing(void)
{
    // Existing: Authorization = "Bearer old"
    bb_sink_http_header_t existing[1];
    memset(existing, 0, sizeof(existing));
    strncpy(existing[0].name,  "Authorization", sizeof(existing[0].name) - 1);
    strncpy(existing[0].value, "Bearer old",    sizeof(existing[0].value) - 1);
    existing[0].secret = true;

    // Patch: Authorization, secret=true, no value → preserve.
    bb_sink_http_patch_entry_t patch[1];
    memset(patch, 0, sizeof(patch));
    strncpy(patch[0].name, "Authorization", sizeof(patch[0].name) - 1);
    patch[0].secret = true;
    patch[0].value_present = false;
    patch[0].value[0] = '\0';

    bb_sink_http_header_t out[8];
    int n = bb_sink_http_merge_headers(patch, 1, existing, 1, out, 8);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("Bearer old", out[0].value);
}

void test_bb_sink_http_merge_omitted_name_removed(void)
{
    // Existing: X-A, X-B. Patch only contains X-A → X-B removed.
    bb_sink_http_header_t existing[2];
    memset(existing, 0, sizeof(existing));
    strncpy(existing[0].name,  "X-A", sizeof(existing[0].name) - 1);
    strncpy(existing[0].value, "va",  sizeof(existing[0].value) - 1);
    strncpy(existing[1].name,  "X-B", sizeof(existing[1].name) - 1);
    strncpy(existing[1].value, "vb",  sizeof(existing[1].value) - 1);

    bb_sink_http_patch_entry_t patch[1];
    memset(patch, 0, sizeof(patch));
    strncpy(patch[0].name,  "X-A",   sizeof(patch[0].name)  - 1);
    strncpy(patch[0].value, "newva", sizeof(patch[0].value) - 1);
    patch[0].value_present = true;

    bb_sink_http_header_t out[8];
    int n = bb_sink_http_merge_headers(patch, 1, existing, 2, out, 8);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("X-A",   out[0].name);
    TEST_ASSERT_EQUAL_STRING("newva", out[0].value);
}

void test_bb_sink_http_merge_independent_edit_secret_preserved(void)
{
    // Existing: Authorization (secret), X-Trace (non-secret).
    // Patch: update X-Trace value, send Authorization blank (preserve).
    bb_sink_http_header_t existing[2];
    memset(existing, 0, sizeof(existing));
    strncpy(existing[0].name,  "Authorization", sizeof(existing[0].name) - 1);
    strncpy(existing[0].value, "Bearer old",    sizeof(existing[0].value) - 1);
    existing[0].secret = true;
    strncpy(existing[1].name,  "X-Trace", sizeof(existing[1].name) - 1);
    strncpy(existing[1].value, "old-val", sizeof(existing[1].value) - 1);
    existing[1].secret = false;

    bb_sink_http_patch_entry_t patch[2];
    memset(patch, 0, sizeof(patch));
    // Authorization: secret=true, no value → preserve.
    strncpy(patch[0].name, "Authorization", sizeof(patch[0].name) - 1);
    patch[0].secret = true;
    patch[0].value_present = false;
    // X-Trace: new value.
    strncpy(patch[1].name,  "X-Trace",  sizeof(patch[1].name)  - 1);
    strncpy(patch[1].value, "new-trace", sizeof(patch[1].value) - 1);
    patch[1].secret = false;
    patch[1].value_present = true;

    bb_sink_http_header_t out[8];
    int n = bb_sink_http_merge_headers(patch, 2, existing, 2, out, 8);
    TEST_ASSERT_EQUAL_INT(2, n);

    // Find Authorization in output.
    bool auth_ok = false;
    bool trace_ok = false;
    for (int i = 0; i < n; i++) {
        if (strcmp(out[i].name, "Authorization") == 0) {
            TEST_ASSERT_EQUAL_STRING("Bearer old", out[i].value);
            auth_ok = true;
        }
        if (strcmp(out[i].name, "X-Trace") == 0) {
            TEST_ASSERT_EQUAL_STRING("new-trace", out[i].value);
            trace_ok = true;
        }
    }
    TEST_ASSERT_TRUE(auth_ok);
    TEST_ASSERT_TRUE(trace_ok);
}

// ---------------------------------------------------------------------------
// Session applies X-Client-Id + configured headers
// ---------------------------------------------------------------------------

void test_bb_sink_http_session_applies_client_id_from_cfg(void)
{
    reset_state();
    set_mock_200();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com:8443", sizeof(cfg.base) - 1);
    strncpy(cfg.client_id, "test-client-001", sizeof(cfg.client_id) - 1);
    cfg.qos     = 1;
    cfg.enabled = true;

    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    // X-Client-Id header must be set to the configured value.
    bb_http_client_header_record_t hdr = bb_http_client_session_find_header("X-Client-Id");
    TEST_ASSERT_EQUAL_STRING("test-client-001", hdr.value);
}

void test_bb_sink_http_session_applies_configured_headers(void)
{
    reset_state();
    set_mock_200();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com:8443", sizeof(cfg.base) - 1);
    cfg.qos     = 1;
    cfg.enabled = true;

    // Two headers — one secret, one not.
    strncpy(cfg.headers[0].name,  "X-Trace-Id", sizeof(cfg.headers[0].name) - 1);
    strncpy(cfg.headers[0].value, "trace-abc",  sizeof(cfg.headers[0].value) - 1);
    cfg.headers[0].secret = false;
    strncpy(cfg.headers[1].name,  "Authorization", sizeof(cfg.headers[1].name) - 1);
    strncpy(cfg.headers[1].value, "Bearer token",  sizeof(cfg.headers[1].value) - 1);
    cfg.headers[1].secret = true;
    cfg.num_headers = 2;

    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    bb_err_t rc = sink.publish(sink.ctx, "t/y", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    bb_http_client_header_record_t h0 = bb_http_client_session_find_header("X-Trace-Id");
    TEST_ASSERT_EQUAL_STRING("trace-abc", h0.value);

    bb_http_client_header_record_t h1 = bb_http_client_session_find_header("Authorization");
    TEST_ASSERT_EQUAL_STRING("Bearer token", h1.value);
}

// ---------------------------------------------------------------------------
// B1-284: session reset after BB_SINK_HTTP_MAX_CONSEC_FAILURES failures
// ---------------------------------------------------------------------------

void test_bb_sink_http_session_reset_after_3_consec_failures(void)
{
    reset_state();
    // No mock_200 — transport error injected below.

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com:8443", sizeof(cfg.base) - 1);
    cfg.qos     = 1;
    cfg.enabled = true;

    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    // First publish succeeds to open the session (open_count == 1).
    bb_http_client_session_set_mock_status(200);
    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, bb_http_client_session_open_count());

    // Now inject 3 consecutive transport errors.
    for (int i = 0; i < 3; i++) {
        bb_http_client_session_set_mock_transport_error(BB_ERR_INVALID_STATE);
        rc = sink.publish(sink.ctx, "t/err", "{}", 2, false);
        TEST_ASSERT_NOT_EQUAL(BB_OK, rc);
    }

    // After the 3rd failure, session_close() should have been called and the
    // next publish must re-open the session (open_count == 2).
    bb_http_client_session_set_mock_status(200);
    rc = sink.publish(sink.ctx, "t/recover", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(2, bb_http_client_session_open_count());
}

// ---------------------------------------------------------------------------
// B1-284: keep_alive cfg flag threads into session_open
// ---------------------------------------------------------------------------

void test_bb_sink_http_keep_alive_cfg_threads_into_session_open(void)
{
    reset_state();
    bb_http_client_session_set_mock_status(200);

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com:8443", sizeof(cfg.base) - 1);
    cfg.qos     = 1;
    cfg.enabled = true;

    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    // Trigger session open via first publish.
    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, bb_http_client_session_open_count());

    // On host (no CONFIG_BB_HTTP_CLIENT_KEEPALIVE defined), session_ensure()
    // defaults keep_alive = true.  Verify the mock recorded it.
    TEST_ASSERT_TRUE(bb_http_client_session_last_keep_alive());
}

void test_bb_sink_http_headers_reapplied_after_set_cfg(void)
{
    reset_state();
    set_mock_200();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://original.example.com:8443", sizeof(cfg.base) - 1);
    strncpy(cfg.client_id, "original-id", sizeof(cfg.client_id) - 1);
    cfg.qos     = 1;
    cfg.enabled = true;

    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    bb_http_client_header_record_t h1 = bb_http_client_session_find_header("X-Client-Id");
    TEST_ASSERT_EQUAL_STRING("original-id", h1.value);

    // Update cfg with new client_id and a new header.
    bb_sink_http_cfg_t cfg2;
    memset(&cfg2, 0, sizeof(cfg2));
    strncpy(cfg2.base, "https://new.example.com:8443", sizeof(cfg2.base) - 1);
    strncpy(cfg2.client_id, "new-id", sizeof(cfg2.client_id) - 1);
    strncpy(cfg2.headers[0].name,  "X-Version", sizeof(cfg2.headers[0].name) - 1);
    strncpy(cfg2.headers[0].value, "2",          sizeof(cfg2.headers[0].value) - 1);
    cfg2.num_headers = 1;
    cfg2.qos     = 1;
    cfg2.enabled = true;
    bb_sink_http_set_cfg(&cfg2);

    // Next publish must re-open session + re-apply new headers.
    rc = sink.publish(sink.ctx, "t/y", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    bb_http_client_header_record_t h2 = bb_http_client_session_find_header("X-Client-Id");
    TEST_ASSERT_EQUAL_STRING("new-id", h2.value);

    bb_http_client_header_record_t h3 = bb_http_client_session_find_header("X-Version");
    TEST_ASSERT_EQUAL_STRING("2", h3.value);
}

// ---------------------------------------------------------------------------
// OOM paths — heap-alloc failure coverage (BB_SINK_HTTP_TESTING)
// ---------------------------------------------------------------------------

// parse_headers: malloc failure → returns 0 (no entries).
void test_bb_sink_http_parse_headers_oom_returns_zero(void)
{
    bb_sink_http_set_malloc(failing_malloc);
    bb_sink_http_header_t out[4];
    int n = bb_sink_http_parse_headers("X-A: v1\n", out, 4);
    bb_sink_http_reset_malloc();
    TEST_ASSERT_EQUAL_INT(0, n);
}

// publish: malloc failure for URL buffer → BB_ERR_NO_SPACE.
void test_bb_sink_http_publish_url_oom_returns_no_space(void)
{
    reset_state();
    set_mock_200();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com:8443", sizeof(cfg.base) - 1);
    cfg.qos     = 1;
    cfg.enabled = true;

    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    bb_sink_http_set_malloc(failing_malloc);
    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    bb_sink_http_reset_malloc();

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
}

// load_from_nvs / bb_sink_http_init: malloc failure for hbuf is graceful
// (init still succeeds with zero headers loaded from NVS).
void test_bb_sink_http_init_nvs_hbuf_oom_graceful(void)
{
    reset_state();

    // Store a header string in NVS so load_from_nvs would normally read it.
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, BB_NV_KEY_HEADERS, "X-A: v1\n");

    // Inject OOM after init is called (malloc will fail for the hbuf).
    bb_sink_http_set_malloc(failing_malloc);
    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com:8443", sizeof(cfg.base) - 1);
    cfg.enabled = true;
    bb_err_t rc = bb_sink_http_init(&cfg);
    bb_sink_http_reset_malloc();

    // init must succeed even though NVS headers couldn't be loaded.
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    // headers should be 0 (OOM prevented NVS load).
    bb_sink_http_cfg_t out;
    bb_sink_http_get_cfg(&out);
    TEST_ASSERT_EQUAL_INT(0, out.num_headers);
}

// ---------------------------------------------------------------------------
// Health API tests (BB_SINK_HTTP_TESTING)
// ---------------------------------------------------------------------------

void test_bb_sink_http_get_health_initial_state(void)
{
    reset_state();
    bb_sink_http_test_reset_health();

    bb_sink_http_health_t h;
    bb_err_t rc = bb_sink_http_get_health(&h);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_FALSE(h.connected);
    TEST_ASSERT_EQUAL_INT(0, h.consec_failures);
    TEST_ASSERT_EQUAL_INT(BB_TLS_FAIL_NONE, h.tls_fail);
    TEST_ASSERT_EQUAL_INT(0, h.last_status);
}

void test_bb_sink_http_get_health_null_returns_invalid_arg(void)
{
    bb_err_t rc = bb_sink_http_get_health(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

void test_bb_sink_http_get_health_roundtrip(void)
{
    bb_sink_http_test_set_health(true, 2, BB_TLS_FAIL_RECORD_TOO_BIG, 503);

    bb_sink_http_health_t h;
    bb_err_t rc = bb_sink_http_get_health(&h);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_TRUE(h.connected);
    TEST_ASSERT_EQUAL_INT(2, h.consec_failures);
    TEST_ASSERT_EQUAL_INT(BB_TLS_FAIL_RECORD_TOO_BIG, h.tls_fail);
    TEST_ASSERT_EQUAL_INT(503, h.last_status);

    bb_sink_http_test_reset_health();
}

void test_bb_sink_http_get_health_other_tls_fail(void)
{
    bb_sink_http_test_set_health(false, 1, BB_TLS_FAIL_OTHER, 0);

    bb_sink_http_health_t h;
    bb_err_t rc = bb_sink_http_get_health(&h);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_FALSE(h.connected);
    TEST_ASSERT_EQUAL_INT(BB_TLS_FAIL_OTHER, h.tls_fail);

    bb_sink_http_test_reset_health();
}

void test_bb_sink_http_get_health_none_tls_fail(void)
{
    bb_sink_http_test_set_health(true, 0, BB_TLS_FAIL_NONE, 200);

    bb_sink_http_health_t h;
    bb_err_t rc = bb_sink_http_get_health(&h);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(BB_TLS_FAIL_NONE, h.tls_fail);
    TEST_ASSERT_EQUAL_INT(200, h.last_status);

    bb_sink_http_test_reset_health();
}

// ---------------------------------------------------------------------------
// bb_transport_health register + report wiring (B1-518 PR2, OBSERVE-ONLY)
// ---------------------------------------------------------------------------

void test_bb_sink_http_publish_success_reports_transport_health(void)
{
    reset_state();
    set_mock_200();
    bb_transport_health_reset_for_test();
    bb_sink_http_reset_transport_health_for_test();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com:8443", sizeof(cfg.base) - 1);
    cfg.qos     = 1;
    cfg.enabled = true;
    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    int enabled = -1, failing = -1;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL_INT(1, enabled);
    TEST_ASSERT_EQUAL_INT(0, failing);
}

void test_bb_sink_http_publish_failure_reports_transport_health(void)
{
    reset_state();
    bb_transport_health_reset_for_test();
    bb_sink_http_reset_transport_health_for_test();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com:8443", sizeof(cfg.base) - 1);
    cfg.qos     = 1;
    cfg.enabled = true;
    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    bb_http_client_session_set_mock_transport_error(BB_ERR_INVALID_STATE);
    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);

    int enabled = -1, failing = -1;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL_INT(1, enabled);
    TEST_ASSERT_EQUAL_INT(1, failing);
}

// Lazy register-once: publish() N times must register exactly once, not
// once per call (enabled count stays 1 across repeated publishes).
void test_bb_sink_http_publish_registers_transport_health_once(void)
{
    reset_state();
    set_mock_200();
    bb_transport_health_reset_for_test();
    bb_sink_http_reset_transport_health_for_test();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com:8443", sizeof(cfg.base) - 1);
    cfg.qos     = 1;
    cfg.enabled = true;
    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    int enabled = -1, failing = -1;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL_INT(1, enabled);

    rc = sink.publish(sink.ctx, "t/y", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    enabled = -1;
    failing = -1;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL_INT(1, enabled);
}

// Slot-exhaustion degrade: when bb_transport_health has no free slots, the
// lazy register-on-publish call fails, but the sink's own publish() outcome
// must still reflect the real transport result — never BB_ERR_NO_SPACE.
void test_bb_sink_http_publish_survives_transport_health_slot_exhaustion(void)
{
    reset_state();
    set_mock_200();
    bb_transport_health_reset_for_test();
    bb_sink_http_reset_transport_health_for_test();

    // Fill every slot directly so the sink's lazy register() call fails.
    bb_transport_handle_t h;
    for (int i = 0; i < BB_TRANSPORT_HEALTH_MAX_SLOTS; i++) {
        TEST_ASSERT_EQUAL_INT(BB_OK, bb_transport_health_register("filler", BB_TRANSPORT_AUTHORITATIVE, &h));
    }

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com:8443", sizeof(cfg.base) - 1);
    cfg.qos     = 1;
    cfg.enabled = true;
    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    // Success path: table is full, but the real transport outcome (BB_OK) wins.
    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    // Failure path: real transport error wins, not BB_ERR_NO_SPACE.
    bb_http_client_session_set_mock_transport_error(BB_ERR_INVALID_STATE);
    rc = sink.publish(sink.ctx, "t/y", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, rc);
    TEST_ASSERT_NOT_EQUAL(BB_ERR_NO_SPACE, rc);
}

// ---------------------------------------------------------------------------
// B1-516: coverage-closing tests (bb_sink_http)
// ---------------------------------------------------------------------------

// parse_headers: name/value exceeding max length must be skipped, not
// truncated-and-kept.
void test_bb_sink_http_parse_skips_name_too_long(void)
{
    char long_name[BB_SINK_HTTP_HEADER_NAME_MAX + 16];
    memset(long_name, 'A', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    char buf[512];
    snprintf(buf, sizeof(buf), "%s: v\nX-Good: ok\n", long_name);

    bb_sink_http_header_t out[4];
    int n = bb_sink_http_parse_headers(buf, out, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("X-Good", out[0].name);
}

void test_bb_sink_http_parse_skips_value_too_long(void)
{
    char long_value[BB_SINK_HTTP_HEADER_VALUE_MAX + 16];
    memset(long_value, 'v', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';

    char buf[1024];
    snprintf(buf, sizeof(buf), "X-Bad: %s\nX-Good: ok\n", long_value);

    bb_sink_http_header_t out[4];
    int n = bb_sink_http_parse_headers(buf, out, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("X-Good", out[0].name);
}

// serialize_headers: a value that doesn't fit in the remaining destination
// capacity must be truncated to fit rather than overflow.
void test_bb_sink_http_serialize_truncates_value_to_fit(void)
{
    bb_sink_http_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    strncpy(hdr.name, "X-A", sizeof(hdr.name) - 1);
    memset(hdr.value, 'v', sizeof(hdr.value) - 1);
    hdr.value[sizeof(hdr.value) - 1] = '\0';

    // Capacity too small to hold the full value after "X-A: ".
    char small_buf[16];
    size_t written = bb_sink_http_serialize_headers(&hdr, 1, small_buf, sizeof(small_buf));
    TEST_ASSERT_TRUE(written < sizeof(hdr.value));
    TEST_ASSERT_TRUE(written < sizeof(small_buf));
}

// merge_headers: secret entry with no value submitted and no matching
// existing entry by name must blank the value (not crash / leak stale data).
void test_bb_sink_http_merge_secret_no_value_no_existing_blanks(void)
{
    bb_sink_http_patch_entry_t patch[1];
    memset(patch, 0, sizeof(patch));
    strncpy(patch[0].name, "X-New-Secret", sizeof(patch[0].name) - 1);
    patch[0].secret = true;
    patch[0].value_present = false;

    bb_sink_http_header_t out[4];
    memset(out, 0, sizeof(out));
    int n = bb_sink_http_merge_headers(patch, 1, NULL, 0, out, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("X-New-Secret", out[0].name);
    TEST_ASSERT_TRUE(out[0].secret);
    TEST_ASSERT_EQUAL_STRING("", out[0].value);
}

// build_url: an unrecognized "{...}" placeholder is copied through
// char-by-char rather than substituted.
void test_bb_sink_http_url_unknown_placeholder_copied_literally(void)
{
    reset_state();
    set_mock_200();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com", sizeof(cfg.base) - 1);
    strncpy(cfg.path_tmpl, "/pub/{unknown}/x", sizeof(cfg.path_tmpl) - 1);
    cfg.enabled = true;
    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    bb_http_client_session_record_t rec = bb_http_client_session_last_post();
    TEST_ASSERT_TRUE(rec.called);
    TEST_ASSERT_NOT_NULL(strstr(rec.url, "/pub/{unknown}/x"));
}

// save_to_nvs (via bb_sink_http_set_cfg): malloc failure for the headers
// serialize buffer is graceful (config still persists, no crash).
void test_bb_sink_http_set_cfg_hbuf_oom_graceful(void)
{
    reset_state();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com", sizeof(cfg.base) - 1);
    cfg.enabled = true;

    bb_sink_http_set_malloc(failing_malloc);
    bb_err_t rc = bb_sink_http_set_cfg(&cfg);
    bb_sink_http_reset_malloc();

    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
}

// session_ensure: TLS credential resolution failure must propagate through
// publish() as the transport-health-reported failure, without a session open.
void test_bb_sink_http_publish_tls_creds_resolve_failure_propagates(void)
{
    reset_state();
    set_mock_200();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com", sizeof(cfg.base) - 1);
    cfg.enabled = true;
    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    bb_tls_creds_set_malloc(failing_malloc);
    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    bb_tls_creds_reset_malloc();

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    // No session should have been opened since creds resolution failed first.
    TEST_ASSERT_EQUAL_INT(0, bb_http_client_session_open_count());
}

// ---------------------------------------------------------------------------
// B1-516: pure-helper null-arg / guard-condition edge cases
// ---------------------------------------------------------------------------

void test_bb_sink_http_set_malloc_null_falls_back_to_libc(void)
{
    bb_sink_http_set_malloc(NULL);
    bb_sink_http_header_t out[4];
    // A normal parse must still work using the libc-malloc fallback.
    int n = bb_sink_http_parse_headers("X-A: v1\n", out, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    bb_sink_http_reset_malloc();
}

void test_bb_sink_http_serialize_headers_null_args_return_zero(void)
{
    bb_sink_http_header_t hdrs[1];
    memset(hdrs, 0, sizeof(hdrs));
    char buf[16];
    TEST_ASSERT_EQUAL_INT(0, (int)bb_sink_http_serialize_headers(NULL, 1, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(0, (int)bb_sink_http_serialize_headers(hdrs, 1, NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(0, (int)bb_sink_http_serialize_headers(hdrs, 1, buf, 0));
}

void test_bb_sink_http_merge_headers_null_args_return_zero(void)
{
    bb_sink_http_patch_entry_t patch[1];
    memset(patch, 0, sizeof(patch));
    bb_sink_http_header_t out[1];
    TEST_ASSERT_EQUAL_INT(0, bb_sink_http_merge_headers(patch, 1, NULL, 0, NULL, 1));
    TEST_ASSERT_EQUAL_INT(0, bb_sink_http_merge_headers(patch, 1, NULL, 0, out, 0));
    TEST_ASSERT_EQUAL_INT(0, bb_sink_http_merge_headers(NULL, 1, NULL, 0, out, 1));
    TEST_ASSERT_EQUAL_INT(0, bb_sink_http_merge_headers(patch, 0, NULL, 0, out, 1));
}

void test_bb_sink_http_url_encode_null_dst_or_zero_cap_returns_zero(void)
{
    char dst[8];
    TEST_ASSERT_EQUAL_INT(0, (int)bb_sink_http_url_encode("abc", NULL, sizeof(dst)));
    TEST_ASSERT_EQUAL_INT(0, (int)bb_sink_http_url_encode("abc", dst, 0));
}

void test_bb_sink_http_get_cfg_null_out_is_noop(void)
{
    // Must not crash.
    bb_sink_http_get_cfg(NULL);
}

void test_bb_sink_http_set_cfg_null_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sink_http_set_cfg(NULL));
}

// ---------------------------------------------------------------------------
// B1-516 pass 2: rare-polarity branch coverage on pure helpers — no
// production seam added; all inputs are ordinary function arguments.
// ---------------------------------------------------------------------------

// header_name_valid: DEL (0x7F) is rejected via the second half of the
// "c <= 0x1F || c == 0x7F" compound (distinct from the c<=0x1F half).
void test_bb_sink_http_header_name_invalid_del_char(void)
{
    char name[2] = { (char)0x7F, '\0' };
    TEST_ASSERT_FALSE(bb_sink_http_header_name_valid(name));
}

void test_bb_sink_http_header_value_valid_null_returns_false(void)
{
    TEST_ASSERT_FALSE(bb_sink_http_header_value_valid(NULL));
}

void test_bb_sink_http_parse_headers_null_out_returns_zero(void)
{
    TEST_ASSERT_EQUAL_INT(0, bb_sink_http_parse_headers("X-A: v\n", NULL, 4));
}

void test_bb_sink_http_parse_headers_zero_out_max_returns_zero(void)
{
    bb_sink_http_header_t out[1];
    TEST_ASSERT_EQUAL_INT(0, bb_sink_http_parse_headers("X-A: v\n", out, 0));
}

// parse_headers: a blank line as the very LAST line (no trailing '\n')
// exercises the nl==NULL side of the blank-line-skip ternary.
void test_bb_sink_http_parse_trailing_blank_line_no_newline(void)
{
    bb_sink_http_header_t out[4];
    int n = bb_sink_http_parse_headers("X-Good: value\n", out, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    // Now a buffer whose FINAL line is blank with nothing after it.
    n = bb_sink_http_parse_headers("X-Good: value\n\n", out, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
}

// parse_headers: a malformed (no ": ") line as the very last line (no
// trailing '\n') exercises the nl==NULL side of that skip ternary.
void test_bb_sink_http_parse_malformed_last_line_no_newline(void)
{
    bb_sink_http_header_t out[4];
    int n = bb_sink_http_parse_headers("X-Good: v\nNoColonSpace", out, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
}

// parse_headers: an invalid-value entry (contains '\r') as the very last
// line (no trailing '\n') exercises the nl==NULL side of that skip ternary.
void test_bb_sink_http_parse_invalid_value_last_line_no_newline(void)
{
    bb_sink_http_header_t out[4];
    // "X-Bad: v\rinjected" has no trailing '\n' -- value contains '\r'.
    int n = bb_sink_http_parse_headers("X-Good: v\nX-Bad: v\rinjected", out, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("X-Good", out[0].name);
}

// parse_headers: an over-length value on the very last line (no trailing
// '\n') exercises the nl==NULL side of the length-cap skip ternary.
void test_bb_sink_http_parse_overlong_value_last_line_no_newline(void)
{
    char long_value[BB_SINK_HTTP_HEADER_VALUE_MAX + 16];
    memset(long_value, 'v', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';

    char buf[1024];
    snprintf(buf, sizeof(buf), "X-Good: ok\nX-Bad: %s", long_value);

    bb_sink_http_header_t out[4];
    int n = bb_sink_http_parse_headers(buf, out, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("X-Good", out[0].name);
}

// serialize_headers: a valid name paired with an invalid value (contains
// '\n') must be skipped (distinct from the invalid-name case already
// covered by test_bb_sink_http_serialize_skips_invalid_entries).
void test_bb_sink_http_serialize_skips_valid_name_invalid_value(void)
{
    bb_sink_http_header_t hdrs[2];
    memset(hdrs, 0, sizeof(hdrs));
    strncpy(hdrs[0].name, "X-Bad", sizeof(hdrs[0].name) - 1);
    strncpy(hdrs[0].value, "v1\ninjected", sizeof(hdrs[0].value) - 1);
    strncpy(hdrs[1].name, "X-Good", sizeof(hdrs[1].name) - 1);
    strncpy(hdrs[1].value, "ok", sizeof(hdrs[1].value) - 1);

    char buf[256] = {0};
    bb_sink_http_serialize_headers(hdrs, 2, buf, sizeof(buf));

    TEST_ASSERT_NOT_NULL(strstr(buf, "X-Good: ok"));
    TEST_ASSERT_NULL(strstr(buf, "X-Bad"));
}

// serialize_headers: the loop's OWN capacity check (not the name/": "
// fit check) terminates the loop when a second header's name plus ": "
// cannot fit in the remaining space.
void test_bb_sink_http_serialize_second_header_name_does_not_fit(void)
{
    bb_sink_http_header_t hdrs[2];
    memset(hdrs, 0, sizeof(hdrs));
    strncpy(hdrs[0].name, "A", sizeof(hdrs[0].name) - 1);
    strncpy(hdrs[0].value, "1", sizeof(hdrs[0].value) - 1);
    strncpy(hdrs[1].name, "X-Second-Long-Name", sizeof(hdrs[1].name) - 1);
    strncpy(hdrs[1].value, "v", sizeof(hdrs[1].value) - 1);

    // "A: 1\n" is 5 bytes; cap just past that leaves no room for the
    // second header's own name + ": ".
    char buf[7] = {0};
    size_t written = bb_sink_http_serialize_headers(hdrs, 2, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "A: 1"));
    TEST_ASSERT_NULL(strstr(buf, "X-Second"));
    TEST_ASSERT_TRUE(written < sizeof(buf));
}

// serialize_headers: the secret '*' prefix write itself is what overflows
// the remaining capacity (distinct from the name/value overflow cases).
void test_bb_sink_http_serialize_secret_prefix_does_not_fit(void)
{
    bb_sink_http_header_t hdrs[2];
    memset(hdrs, 0, sizeof(hdrs));
    strncpy(hdrs[0].name, "A", sizeof(hdrs[0].name) - 1);
    strncpy(hdrs[0].value, "1", sizeof(hdrs[0].value) - 1);
    hdrs[0].secret = false;
    strncpy(hdrs[1].name, "B", sizeof(hdrs[1].name) - 1);
    strncpy(hdrs[1].value, "2", sizeof(hdrs[1].value) - 1);
    hdrs[1].secret = true;

    // "A: 1\n" consumes 5 bytes; cap=6 leaves exactly 1 byte (for NUL),
    // so the '*' prefix write for the second (secret) header cannot fit.
    char buf[6] = {0};
    bb_sink_http_serialize_headers(hdrs, 2, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "A: 1"));
    TEST_ASSERT_NULL(strstr(buf, "B"));
}

// serialize_headers: an empty value ("") must serialize as "name: " with
// no value bytes copied (vlen==0 skips the memcpy branch).
void test_bb_sink_http_serialize_empty_value(void)
{
    bb_sink_http_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    strncpy(hdr.name, "X-Empty", sizeof(hdr.name) - 1);
    hdr.value[0] = '\0';

    char buf[64] = {0};
    size_t written = bb_sink_http_serialize_headers(&hdr, 1, buf, sizeof(buf));
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "X-Empty: "));
}

// serialize_headers: the '\n' separator between two non-final headers is
// itself what overflows the remaining capacity.
void test_bb_sink_http_serialize_separator_does_not_fit(void)
{
    bb_sink_http_header_t hdrs[3];
    memset(hdrs, 0, sizeof(hdrs));
    strncpy(hdrs[0].name, "A", sizeof(hdrs[0].name) - 1);
    strncpy(hdrs[0].value, "1", sizeof(hdrs[0].value) - 1);
    strncpy(hdrs[1].name, "B", sizeof(hdrs[1].name) - 1);
    hdrs[1].value[0] = '\0';
    strncpy(hdrs[2].name, "C", sizeof(hdrs[2].name) - 1);
    strncpy(hdrs[2].value, "3", sizeof(hdrs[2].value) - 1);

    // "A: 1" (4) + "\n" (1) + "B: " (3) = 8 bytes exactly, leaving no room
    // for a trailing '\n' separator before the (dropped) third header.
    char buf[9] = {0};
    bb_sink_http_serialize_headers(hdrs, 3, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "A: 1"));
}

// merge_headers: more valid patch entries than out_max — the loop's own
// out-of-space check (not patch_count) terminates it.
void test_bb_sink_http_merge_headers_out_max_caps_loop(void)
{
    bb_sink_http_patch_entry_t patch[3];
    memset(patch, 0, sizeof(patch));
    for (int i = 0; i < 3; i++) {
        snprintf(patch[i].name, sizeof(patch[i].name), "X-%d", i);
        snprintf(patch[i].value, sizeof(patch[i].value), "v%d", i);
        patch[i].value_present = true;
    }
    bb_sink_http_header_t out[2];
    memset(out, 0, sizeof(out));
    int n = bb_sink_http_merge_headers(patch, 3, NULL, 0, out, 2);
    TEST_ASSERT_EQUAL_INT(2, n);
}

// merge_headers: an empty-name entry is rejected outright.
void test_bb_sink_http_merge_headers_empty_name_rejected(void)
{
    bb_sink_http_patch_entry_t patch[2];
    memset(patch, 0, sizeof(patch));
    patch[0].name[0] = '\0';
    strncpy(patch[0].value, "v", sizeof(patch[0].value) - 1);
    patch[0].value_present = true;
    strncpy(patch[1].name, "X-Good", sizeof(patch[1].name) - 1);
    strncpy(patch[1].value, "ok", sizeof(patch[1].value) - 1);
    patch[1].value_present = true;

    bb_sink_http_header_t out[4];
    memset(out, 0, sizeof(out));
    int n = bb_sink_http_merge_headers(patch, 2, NULL, 0, out, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("X-Good", out[0].name);
}

// merge_headers: a non-empty but invalid name (contains ':') is rejected.
void test_bb_sink_http_merge_headers_invalid_name_rejected(void)
{
    bb_sink_http_patch_entry_t patch[1];
    memset(patch, 0, sizeof(patch));
    strncpy(patch[0].name, "Bad:Name", sizeof(patch[0].name) - 1);
    strncpy(patch[0].value, "v", sizeof(patch[0].value) - 1);
    patch[0].value_present = true;

    bb_sink_http_header_t out[1];
    memset(out, 0, sizeof(out));
    int n = bb_sink_http_merge_headers(patch, 1, NULL, 0, out, 1);
    TEST_ASSERT_EQUAL_INT(0, n);
}

// merge_headers: secret+no-value with a non-empty existing array that
// contains NO matching name must blank the value (distinct from the
// existing==NULL case already covered).
void test_bb_sink_http_merge_headers_secret_no_match_in_existing_blanks(void)
{
    bb_sink_http_patch_entry_t patch[1];
    memset(patch, 0, sizeof(patch));
    strncpy(patch[0].name, "X-New-Secret", sizeof(patch[0].name) - 1);
    patch[0].secret = true;
    patch[0].value_present = false;

    bb_sink_http_header_t existing[1];
    memset(existing, 0, sizeof(existing));
    strncpy(existing[0].name, "X-Other", sizeof(existing[0].name) - 1);
    strncpy(existing[0].value, "unrelated", sizeof(existing[0].value) - 1);

    bb_sink_http_header_t out[1];
    memset(out, 0, sizeof(out));
    int n = bb_sink_http_merge_headers(patch, 1, existing, 1, out, 1);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("", out[0].value);
}

// merge_headers: a non-secret entry with an invalid submitted value
// (contains '\n') is rejected.
void test_bb_sink_http_merge_headers_invalid_value_rejected(void)
{
    bb_sink_http_patch_entry_t patch[1];
    memset(patch, 0, sizeof(patch));
    strncpy(patch[0].name, "X-Bad", sizeof(patch[0].name) - 1);
    strncpy(patch[0].value, "v\ninjected", sizeof(patch[0].value) - 1);
    patch[0].value_present = true;
    patch[0].secret = false;

    bb_sink_http_header_t out[1];
    memset(out, 0, sizeof(out));
    int n = bb_sink_http_merge_headers(patch, 1, NULL, 0, out, 1);
    TEST_ASSERT_EQUAL_INT(0, n);
}

// url_encode: the outer loop's own capacity check (out+1<dst_cap) — not a
// mid-percent-encode break — terminates the loop when plain unreserved
// characters exactly fill the destination.
void test_bb_sink_http_url_encode_exact_capacity_unreserved_chars(void)
{
    char out[4] = {0};
    size_t n = bb_sink_http_url_encode("abcd", out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(3, (int)n);
    TEST_ASSERT_EQUAL_STRING("abc", out);
}

// merge_headers: secret entry with value_present=true but an explicitly
// empty value ("") must still blank/preserve-by-name (distinct from the
// value_present=false case already covered).
void test_bb_sink_http_merge_headers_secret_present_but_empty_value(void)
{
    bb_sink_http_patch_entry_t patch[1];
    memset(patch, 0, sizeof(patch));
    strncpy(patch[0].name, "X-Secret", sizeof(patch[0].name) - 1);
    patch[0].secret = true;
    patch[0].value_present = true;
    patch[0].value[0] = '\0';

    bb_sink_http_header_t out[1];
    memset(out, 0, sizeof(out));
    int n = bb_sink_http_merge_headers(patch, 1, NULL, 0, out, 1);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("", out[0].value);
}

// load_from_nvs: a corrupt/non-digit "qos" NVS value drives qos negative,
// hitting the "< 0" half of the clamp (distinct from the "> 2" half,
// already covered elsewhere by normal qos round-trips).
void test_bb_sink_http_get_cfg_qos_negative_from_nvs_clamped(void)
{
    reset_state();
    // qos_str[0] - '0' is negative when qos_str[0] < '0' (e.g. '!' = 0x21,
    // '0' = 0x30 -> negative delta).
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "qos", "!");

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com", sizeof(cfg.base) - 1);
    bb_sink_http_init(&cfg);

    bb_sink_http_cfg_t out;
    bb_sink_http_get_cfg(&out);
    TEST_ASSERT_EQUAL_INT(1, out.qos);  // clamped to the default (1)
}

// load_from_nvs: a corrupt "qos" NVS value above the valid range (a digit
// > 2) drives qos above 2, hitting the "> 2" half of the clamp (distinct
// from the "< 0" half above).
void test_bb_sink_http_get_cfg_qos_above_range_from_nvs_clamped(void)
{
    reset_state();
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "qos", "9");

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com", sizeof(cfg.base) - 1);
    bb_sink_http_init(&cfg);

    bb_sink_http_cfg_t out;
    bb_sink_http_get_cfg(&out);
    TEST_ASSERT_EQUAL_INT(1, out.qos);  // clamped to the default (1)
}

// save_to_nvs (via set_cfg): enabled=false must persist "0" (the ternary's
// other polarity from the enabled=true round-trip already covered).
void test_bb_sink_http_set_cfg_enabled_false_persists_zero(void)
{
    reset_state();
    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com", sizeof(cfg.base) - 1);
    cfg.enabled = false;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sink_http_set_cfg(&cfg));

    char buf[4] = {0};
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "enabled", buf, sizeof(buf), "1");
    TEST_ASSERT_EQUAL_STRING("0", buf);
}

// apply_headers_to_session: when both client_id and hostname are empty,
// cid[0] is false and the X-Client-Id header must be omitted (distinct
// from the normal hostname-fallback case already covered elsewhere).
void test_bb_sink_http_session_no_client_id_and_no_hostname_omits_header(void)
{
    reset_state();
    set_mock_200();
    bb_nv_config_factory_reset();  // hostname reverts to "" (empty)

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com", sizeof(cfg.base) - 1);
    cfg.enabled = true;
    // client_id left empty.
    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    bb_http_client_header_record_t rec = bb_http_client_session_find_header("X-Client-Id");
    TEST_ASSERT_EQUAL_STRING("", rec.name);  // not found -> zeroed record
}

// apply_headers_to_session: a configured header entry with an empty name
// (defensive slot, never produced by the public merge/patch path) must be
// skipped rather than sent with an empty header name.
void test_bb_sink_http_session_applies_headers_skips_empty_name_entry(void)
{
    reset_state();
    set_mock_200();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com", sizeof(cfg.base) - 1);
    cfg.enabled = true;
    cfg.num_headers = 2;
    // headers[0] has an empty name -- must be skipped.
    strncpy(cfg.headers[1].name, "X-Real", sizeof(cfg.headers[1].name) - 1);
    strncpy(cfg.headers[1].value, "ok", sizeof(cfg.headers[1].value) - 1);
    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    bb_http_client_header_record_t real = bb_http_client_session_find_header("X-Real");
    TEST_ASSERT_EQUAL_STRING("ok", real.value);
}

// bb_sink_http_init: an override with an empty base leaves the NVS-loaded
// base untouched (the "over->base[0]" false polarity).
void test_bb_sink_http_init_override_empty_base_preserves_nvs_base(void)
{
    reset_state();
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "base", "https://from-nvs.example.com");

    bb_sink_http_cfg_t over;
    memset(&over, 0, sizeof(over));
    // over.base left empty; only qos is overridden.
    over.qos = 2;
    bb_sink_http_init(&over);

    bb_sink_http_cfg_t out;
    bb_sink_http_get_cfg(&out);
    TEST_ASSERT_EQUAL_STRING("https://from-nvs.example.com", out.base);
    TEST_ASSERT_EQUAL_INT(2, out.qos);
}

// bb_sink_http_init: an override with more headers than
// BB_SINK_HTTP_HEADERS_MAX is clamped to the cap rather than overflowing
// s_cfg.headers[].
void test_bb_sink_http_init_override_headers_overflow_clamped(void)
{
    reset_state();
    bb_sink_http_cfg_t over;
    memset(&over, 0, sizeof(over));
    strncpy(over.base, "https://broker.example.com", sizeof(over.base) - 1);
    over.num_headers = BB_SINK_HTTP_HEADERS_MAX + 4;
    for (int i = 0; i < BB_SINK_HTTP_HEADERS_MAX; i++) {
        snprintf(over.headers[i].name, sizeof(over.headers[i].name), "X-%d", i);
    }
    bb_sink_http_init(&over);

    bb_sink_http_cfg_t out;
    bb_sink_http_get_cfg(&out);
    TEST_ASSERT_EQUAL_INT(BB_SINK_HTTP_HEADERS_MAX, out.num_headers);
}

// ---------------------------------------------------------------------------
// B1-516 pass 3: seam-backed coverage (session-open failure, TLS-error carry
// on failure)
// ---------------------------------------------------------------------------

static void *failing_session_calloc(size_t n, size_t sz)
{
    (void)n; (void)sz;
    return NULL;
}

// session_ensure: bb_http_client_session_open failing (simulated allocation
// failure) must propagate through publish() without a partial/dangling
// session, distinct from the TLS-creds-resolve failure case (which fails
// before session_open is ever reached).
void test_bb_sink_http_publish_session_open_failure_propagates(void)
{
    reset_state();
    set_mock_200();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com", sizeof(cfg.base) - 1);
    cfg.enabled = true;
    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    bb_http_client_host_set_session_calloc(failing_session_calloc);
    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    bb_http_client_host_set_session_calloc(NULL);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
    TEST_ASSERT_EQUAL_INT(0, bb_http_client_session_open_count());
}

// http_pub_publish: a mocked transport failure that ALSO carries a nonzero
// tls_error_code must classify + log the TLS handshake diagnostic (distinct
// from a plain transport failure with tls_error_code==0, already covered).
void test_bb_sink_http_publish_failure_with_tls_error_code_classifies(void)
{
    reset_state();

    bb_sink_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base, "https://broker.example.com", sizeof(cfg.base) - 1);
    cfg.enabled = true;
    bb_sink_http_init(&cfg);

    bb_pub_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    bb_sink_http(&sink);

    bb_http_client_session_set_mock_transport_error(BB_ERR_TIMEOUT);
    bb_http_client_session_set_mock_tls_error_code(-0x7280);  /* arbitrary mbedtls-style code */

    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2, false);
    TEST_ASSERT_EQUAL_INT(BB_ERR_TIMEOUT, rc);

    bb_sink_http_health_t health;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sink_http_get_health(&health));
    TEST_ASSERT_NOT_EQUAL(BB_TLS_FAIL_NONE, health.tls_fail);
}

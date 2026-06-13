// Tests for bb_sink_http:
// - url_encode: slash → %2F, unreserved chars pass through
// - sink publish() builds expected URL (base + path_template substitution)
//   and calls bb_http_client_session_post — verified via
//   bb_http_client_session_last_post (session API, not one-shot post)
// - custom path_tmpl override
// - disabled → no POST
// - session reused across multiple publishes (same handle, not re-opened)
#include "unity.h"
#include "bb_sink_http.h"
#include "bb_nv.h"
#include "../../platform/host/bb_http_client/bb_http_client_host.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

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
    rc = sink.publish(sink.ctx, topic, payload, (int)strlen(payload));
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
    bb_err_t rc = sink.publish(sink.ctx, topic, payload, (int)strlen(payload));
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

    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2);
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
    bb_err_t rc = sink.publish(sink.ctx, "t/one", "{\"n\":1}", 6);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    bb_http_client_session_record_t rec1 = bb_http_client_session_last_post();
    TEST_ASSERT_TRUE(rec1.called);
    TEST_ASSERT_NOT_NULL(strstr(rec1.url, "t%2Fone"));

    rc = sink.publish(sink.ctx, "t/two", "{\"n\":2}", 6);
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
    bb_err_t rc = sink.publish(sink.ctx, "t/x", "{}", 2);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    // Change config — session must close and reopen on next publish.
    bb_sink_http_cfg_t cfg2;
    memset(&cfg2, 0, sizeof(cfg2));
    strncpy(cfg2.base, "https://new.example.com:8443", sizeof(cfg2.base) - 1);
    cfg2.qos     = 0;
    cfg2.enabled = true;
    bb_sink_http_set_cfg(&cfg2);

    rc = sink.publish(sink.ctx, "t/y", "{}", 2);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);

    bb_http_client_session_record_t rec = bb_http_client_session_last_post();
    TEST_ASSERT_TRUE(rec.called);
    // URL should now use the new base.
    TEST_ASSERT_NOT_NULL(strstr(rec.url, "new.example.com"));
}

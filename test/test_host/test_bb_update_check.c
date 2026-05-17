#include "unity.h"
#include "bb_update_check.h"
#include "bb_update_check_internal.h"
#include "bb_release_manifest.h"
#include "bb_http_client_host.h"
#include "bb_event.h"
#include "bb_event_test.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Hook invocation counters (reset in reset_world).
static int g_pause_calls  = 0;
static int g_resume_calls = 0;
static int g_last_hook    = 0; // 1=pause, 2=resume (tracks order)
static int g_pause_order  = 0;
static int g_resume_order = 0;
static int g_hook_seq     = 0;

static void hook_pause(void)  { g_hook_seq++; g_pause_calls++;  g_pause_order  = g_hook_seq; g_last_hook = 1; }
static void hook_resume(void) { g_hook_seq++; g_resume_calls++; g_resume_order = g_hook_seq; g_last_hook = 2; }

// Mock parser used by the override test.
static int g_mock_parser_calls = 0;
static bb_err_t mock_parser(const char *body, size_t body_len,
                            const char *board, char *out_tag, size_t tag_sz,
                            char *out_url, size_t url_sz)
{
    (void)body; (void)body_len; (void)board;
    g_mock_parser_calls++;
    strncpy(out_tag, "v9.9.9", tag_sz - 1);
    out_tag[tag_sz - 1] = '\0';
    strncpy(out_url, "http://mock.example/firmware.bin", url_sz - 1);
    out_url[url_sz - 1] = '\0';
    return BB_OK;
}

// Parser that always returns a version equal to the running version.
// bb_system_get_version() on host returns "0.0.0-host" (parses as 0.0.0).
static bb_err_t same_version_parser(const char *body, size_t body_len,
                                    const char *board, char *out_tag, size_t tag_sz,
                                    char *out_url, size_t url_sz)
{
    (void)body; (void)body_len; (void)board;
    strncpy(out_tag, "v0.0.0", tag_sz - 1);
    out_tag[tag_sz - 1] = '\0';
    strncpy(out_url, "http://mock.example/firmware.bin", url_sz - 1);
    out_url[url_sz - 1] = '\0';
    return BB_OK;
}

static const char *VALID_BODY =
    "{\"tag_name\":\"v9.9.9\","
    "\"assets\":[{\"name\":\"firmware.bin\","
    "\"browser_download_url\":\"https://example.com/firmware.bin\"}]}";

static const char *SAME_BODY_TEMPLATE =
    "{\"tag_name\":\"%s\","
    "\"assets\":[{\"name\":\"firmware.bin\","
    "\"browser_download_url\":\"https://example.com/firmware.bin\"}]}";

static void reset_world(void)
{
    bb_update_check_reset_for_test();
    bb_http_client_clear_mock();
    bb_event_reset_for_test();
    bb_event_init(NULL);
    g_mock_parser_calls = 0;
    g_pause_calls  = 0;
    g_resume_calls = 0;
    g_last_hook    = 0;
    g_pause_order  = 0;
    g_resume_order = 0;
    g_hook_seq     = 0;
}

// ---------------------------------------------------------------------------
// init / accessors
// ---------------------------------------------------------------------------

void test_bb_update_check_init_idempotent(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_init(NULL));
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_init(NULL));
}

void test_bb_update_check_init_with_cfg_uses_overrides(void)
{
    reset_world();
    bb_update_check_cfg_t cfg = { .interval_s = 60, .post_initial = true };
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_init(&cfg));
}

void test_bb_update_check_get_status_before_init_returns_invalid_state(void)
{
    reset_world();
    bb_update_check_status_t st;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_update_check_get_status(&st));
}

void test_bb_update_check_get_status_null_out_returns_invalid_arg(void)
{
    reset_world();
    bb_update_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_update_check_get_status(NULL));
}

void test_bb_update_check_set_releases_url_validates(void)
{
    reset_world();
    bb_update_check_init(NULL);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_update_check_set_releases_url(NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_update_check_set_releases_url(""));

    char too_long[300];
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_update_check_set_releases_url(too_long));

    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_set_releases_url("http://example.com/r.json"));
}

void test_bb_update_check_set_releases_url_before_init_returns_invalid_state(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                      bb_update_check_set_releases_url("http://example.com/r.json"));
}

void test_bb_update_check_set_parser_before_init_returns_invalid_state(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_update_check_set_parser(mock_parser));
}

void test_bb_update_check_set_parser_null_restores_default(void)
{
    reset_world();
    bb_update_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_set_parser(mock_parser));
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_set_parser(NULL));
}

// ---------------------------------------------------------------------------
// run_one — transitions, sticky failure, post_initial
// ---------------------------------------------------------------------------

void test_bb_update_check_run_one_before_init_returns_invalid_arg(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_update_check_run_one());
}

void test_bb_update_check_run_one_without_url_returns_invalid_state(void)
{
    reset_world();
    bb_update_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_update_check_run_one());
}

void test_bb_update_check_now_without_url_returns_invalid_state(void)
{
    reset_world();
    bb_update_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_update_check_now());
}

void test_bb_update_check_now_before_init_returns_invalid_arg(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_update_check_now());
}

void test_bb_update_check_run_one_newer_release_flips_available(void)
{
    reset_world();
    bb_update_check_init(NULL);
    /* Host bb_system_get_version returns "0.0.0" so v9.9.9 is newer. */
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());

    bb_update_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_get_status(&st));
    TEST_ASSERT_TRUE(st.available);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
    TEST_ASSERT_EQUAL_STRING("https://example.com/firmware.bin", st.download_url);
    TEST_ASSERT_NOT_EQUAL(0, st.last_check_us);
}

void test_bb_update_check_run_one_same_version_keeps_unavailable(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    /* "0.0.0" matches host fallback running version; not newer. */
    char body[256];
    snprintf(body, sizeof(body), SAME_BODY_TEMPLATE, "v0.0.0");
    bb_http_client_set_mock_response(body, strlen(body), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_TRUE(st.last_check_ok);
}

void test_bb_update_check_run_one_transport_failure_sticky(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);

    bb_err_t err = bb_update_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_FALSE(st.last_check_ok);
    TEST_ASSERT_NOT_EQUAL(0, st.last_check_us);
}

void test_bb_update_check_run_one_http_404_sticky_failure(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_response("Not Found", 9, 404);

    bb_err_t err = bb_update_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_FALSE(st.last_check_ok);
}

void test_bb_update_check_run_one_parse_failure_sticky(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_response("not json", 8, 200);

    bb_err_t err = bb_update_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_FALSE(st.last_check_ok);
}

void test_bb_update_check_run_one_recovers_after_failure(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");

    /* First: transport error. */
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);
    bb_update_check_run_one();
    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_FALSE(st.last_check_ok);

    /* Then: successful response clears sticky. */
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());
    bb_update_check_get_status(&st);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_TRUE(st.available);
}

void test_bb_update_check_run_one_custom_parser_invoked(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_parser(mock_parser);
    bb_http_client_set_mock_response("anything", 8, 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());
    TEST_ASSERT_EQUAL(1, g_mock_parser_calls);

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
}

void test_bb_update_check_post_initial_publishes_on_first_check(void)
{
    reset_world();
    bb_update_check_cfg_t cfg = { .interval_s = 60, .post_initial = true };
    bb_update_check_init(&cfg);
    bb_update_check_set_releases_url("http://example.com/r.json");
    /* Same-as-current body: no transition, but post_initial should publish. */
    char body[256];
    snprintf(body, sizeof(body), SAME_BODY_TEMPLATE, "v0.0.0");
    bb_http_client_set_mock_response(body, strlen(body), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_TRUE(st.last_check_ok);
}

void test_bb_update_check_dev_tag_treated_as_older(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    /* "dev" tag is older than running "0.0.0" -> not available. */
    char body[256];
    snprintf(body, sizeof(body), SAME_BODY_TEMPLATE, "dev1.2.3");
    bb_http_client_set_mock_response(body, strlen(body), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_TRUE(st.last_check_ok);
}

void test_bb_update_check_run_one_newer_to_same_transitions_back(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");

    /* First: newer release; available -> true. */
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());
    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_TRUE(st.available);

    /* Then: same-as-running tag; available -> false (transition). */
    char body[256];
    snprintf(body, sizeof(body), SAME_BODY_TEMPLATE, "v0.0.0");
    bb_http_client_set_mock_response(body, strlen(body), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());
    bb_update_check_get_status(&st);
    TEST_ASSERT_FALSE(st.available);
}

void test_bb_update_check_now_drives_a_check(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_now());

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_TRUE(st.available);
}

void test_bb_update_check_custom_parser_transport_error(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_parser(mock_parser);
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);

    bb_err_t err = bb_update_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_FALSE(st.last_check_ok);
    TEST_ASSERT_NOT_EQUAL(0, st.last_check_us);
}

static bb_err_t failing_parser(const char *body, size_t body_len,
                               const char *board, char *out_tag, size_t tag_sz,
                               char *out_url, size_t url_sz)
{
    (void)body; (void)body_len; (void)board;
    (void)out_tag; (void)tag_sz; (void)out_url; (void)url_sz;
    return BB_ERR_NOT_FOUND;
}

void test_bb_update_check_custom_parser_parse_failure(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_parser(failing_parser);
    bb_http_client_set_mock_response("anything", 8, 200);

    bb_err_t err = bb_update_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_FALSE(st.last_check_ok);
    TEST_ASSERT_NOT_EQUAL(0, st.last_check_us);
}

void test_bb_update_check_custom_parser_http_404(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_parser(mock_parser);
    bb_http_client_set_mock_response("Not Found", 9, 404);

    bb_err_t err = bb_update_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_FALSE(st.last_check_ok);
}

void test_bb_update_check_custom_parser_body_exceeds_16k(void)
{
    // A response body larger than CUSTOM_PARSER_BUF_SIZE (16384 bytes) exercises
    // the buf_chunk_cb overflow path: copy==0 (L210 false branch), overflow flag
    // set (L214 true), and the early-return on a subsequent chunk (L207 true).
    // mock_parser ignores the body so the parse succeeds regardless.
    reset_world();
    bb_update_check_cfg_t cfg = { .interval_s = 60, .post_initial = false };
    bb_update_check_init(&cfg);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_parser(mock_parser);

    // 16641 bytes > 16384 (CUSTOM_PARSER_BUF_SIZE) + 256-byte chunk padding
    const size_t big = 16641;
    char *body = (char *)malloc(big);
    TEST_ASSERT_NOT_NULL(body);
    memset(body, 'x', big - 1);
    body[big - 1] = '\0';
    bb_http_client_set_mock_response(body, big, 200);

    bb_err_t err = bb_update_check_run_one();
    free(body);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
}

void test_bb_update_check_custom_parser_post_initial_publishes(void)
{
    // Custom-parser path with post_initial=true and no version transition:
    // same_version_parser returns v0.0.0 == running, so transition=false but
    // initial_publish=true — exercises the `|| initial_publish` branch of the
    // `if (transition || initial_publish)` guard (L370-372).
    reset_world();
    bb_update_check_cfg_t cfg = { .interval_s = 60, .post_initial = true };
    bb_update_check_init(&cfg);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_parser(same_version_parser);
    bb_http_client_set_mock_response("x", 1, 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_TRUE(st.last_check_ok);

    // Second run: first_call=false, transition=false, initial_publish=false.
    // Exercises the `&&` short-circuit at L370 (first_call=false path) and
    // the `if (transition || initial_publish)` false branch at L371.
    bb_http_client_set_mock_response("x", 1, 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());
    bb_update_check_get_status(&st);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_TRUE(st.last_check_ok);
}

// ---------------------------------------------------------------------------
// bb_update_check_set_hooks
// ---------------------------------------------------------------------------

void test_bb_update_check_set_hooks_before_init_returns_invalid_state(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                      bb_update_check_set_hooks(hook_pause, hook_resume));
}

void test_bb_update_check_set_hooks_null_clears(void)
{
    reset_world();
    bb_update_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_set_hooks(hook_pause, hook_resume));
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_set_hooks(NULL, NULL));
    // After clearing, a successful run must not call either hook.
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());
    TEST_ASSERT_EQUAL(0, g_pause_calls);
    TEST_ASSERT_EQUAL(0, g_resume_calls);
}

void test_bb_update_check_hooks_called_in_order_on_success(void)
{
    // Default (github streaming) parser path: pause before fetch, resume after.
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_hooks(hook_pause, hook_resume);
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());

    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(1, g_resume_calls);
    // pause must have been called before resume
    TEST_ASSERT_TRUE(g_pause_order < g_resume_order);
    TEST_ASSERT_EQUAL(2, g_last_hook);
}

void test_bb_update_check_hooks_resume_fires_on_transport_error(void)
{
    // Default path: resume must fire even when the fetch fails.
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_hooks(hook_pause, hook_resume);
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);

    bb_err_t err = bb_update_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);

    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(1, g_resume_calls);
    TEST_ASSERT_TRUE(g_pause_order < g_resume_order);
}

void test_bb_update_check_hooks_resume_fires_on_parse_error(void)
{
    // Default path: resume must fire even when the parse step fails.
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_hooks(hook_pause, hook_resume);
    bb_http_client_set_mock_response("not json", 8, 200);

    bb_err_t err = bb_update_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);

    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(1, g_resume_calls);
    TEST_ASSERT_TRUE(g_pause_order < g_resume_order);
}

void test_bb_update_check_hooks_called_once_per_run(void)
{
    // Two consecutive runs each trigger exactly one pause+resume pair.
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_hooks(hook_pause, hook_resume);

    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());
    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(1, g_resume_calls);

    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());
    TEST_ASSERT_EQUAL(2, g_pause_calls);
    TEST_ASSERT_EQUAL(2, g_resume_calls);
}

void test_bb_update_check_hooks_custom_parser_success(void)
{
    // Custom parser path: hooks must also fire.
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_parser(mock_parser);
    bb_update_check_set_hooks(hook_pause, hook_resume);
    bb_http_client_set_mock_response("anything", 8, 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());
    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(1, g_resume_calls);
    TEST_ASSERT_TRUE(g_pause_order < g_resume_order);
}

void test_bb_update_check_hooks_custom_parser_transport_error(void)
{
    // Custom parser path: resume fires even on transport failure.
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_parser(mock_parser);
    bb_update_check_set_hooks(hook_pause, hook_resume);
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);

    bb_err_t err = bb_update_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(1, g_resume_calls);
    TEST_ASSERT_TRUE(g_pause_order < g_resume_order);
}

void test_bb_update_check_hooks_custom_parser_parse_error(void)
{
    // Custom parser path: resume fires even when parse fails.
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_parser(failing_parser);
    bb_update_check_set_hooks(hook_pause, hook_resume);
    bb_http_client_set_mock_response("anything", 8, 200);

    bb_err_t err = bb_update_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(1, g_resume_calls);
    TEST_ASSERT_TRUE(g_pause_order < g_resume_order);
}

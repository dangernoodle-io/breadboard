#include "unity.h"
#include "bb_update_check.h"
#include "bb_update_check_internal.h"
#include "bb_release_manifest.h"
#include "bb_http_client_host.h"
#include "bb_event.h"
#include "bb_event_ring.h"
#include "bb_event_test.h"
#include "bb_nv.h"
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

static bool g_pause_returns = true;
static bool hook_pause(void)  { g_hook_seq++; g_pause_calls++;  g_pause_order  = g_hook_seq; g_last_hook = 1; return g_pause_returns; }
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

// Body with a taipanminer-tdongle-s3.bin asset (no firmware.bin).
static const char *TDONGLE_BODY =
    "{\"tag_name\":\"v9.9.9\","
    "\"assets\":[{\"name\":\"taipanminer-tdongle-s3.bin\","
    "\"browser_download_url\":\"https://example.com/taipanminer-tdongle-s3.bin\"}]}";

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
    g_pause_returns = true;
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

// pause returning false: fetch is skipped and resume is NOT called.
void test_bb_update_check_pause_returns_false_skips_fetch(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_hooks(hook_pause, hook_resume);
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);

    g_pause_returns = false;
    bb_err_t err = bb_update_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(0, g_resume_calls);
}

void test_bb_update_check_pause_returns_false_custom_parser_skips_fetch(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_parser(mock_parser);
    bb_update_check_set_hooks(hook_pause, hook_resume);
    bb_http_client_set_mock_response("anything", 8, 200);

    g_pause_returns = false;
    bb_err_t err = bb_update_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(0, g_resume_calls);
}

// ---------------------------------------------------------------------------
// bb_update_check_set_firmware_board
// ---------------------------------------------------------------------------

void test_bb_update_check_set_firmware_board_before_init_returns_invalid_state(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                      bb_update_check_set_firmware_board("taipanminer-tdongle-s3"));
}

void test_bb_update_check_set_firmware_board_too_long_returns_invalid_arg(void)
{
    reset_world();
    bb_update_check_init(NULL);
    // 64 chars is exactly BOARD_MAX — must be rejected (>= BOARD_MAX).
    char too_long[65];
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_update_check_set_firmware_board(too_long));
}

void test_bb_update_check_set_firmware_board_null_clears_to_default(void)
{
    // After setting a board and then passing NULL, a run with the default
    // firmware.bin body should match again.
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_update_check_set_firmware_board("taipanminer-tdongle-s3"));
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_set_firmware_board(NULL));

    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
}

void test_bb_update_check_set_firmware_board_empty_string_clears_to_default(void)
{
    // Empty string "" reverts to BOARD_NAME_FALLBACK, same as NULL.
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_update_check_set_firmware_board("taipanminer-tdongle-s3"));
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_set_firmware_board(""));

    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
}

void test_bb_update_check_firmware_board_matches_named_asset(void)
{
    // Setting "taipanminer-tdongle-s3" causes the parser to look for
    // "taipanminer-tdongle-s3.bin" — the TDONGLE_BODY has exactly that asset.
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_update_check_set_firmware_board("taipanminer-tdongle-s3"));

    bb_http_client_set_mock_response(TDONGLE_BODY, strlen(TDONGLE_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_TRUE(st.available);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
    TEST_ASSERT_EQUAL_STRING("https://example.com/taipanminer-tdongle-s3.bin",
                             st.download_url);
}

void test_bb_update_check_firmware_board_default_does_not_match_named_asset(void)
{
    // Without setting a board, the default "firmware" fallback does NOT match
    // "taipanminer-tdongle-s3.bin" — parse fails with BB_ERR_NOT_FOUND.
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    // Do NOT call set_firmware_board — use default.

    bb_http_client_set_mock_response(TDONGLE_BODY, strlen(TDONGLE_BODY), 200);
    bb_err_t err = bb_update_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);

    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_FALSE(st.last_check_ok);
}

void test_bb_update_check_firmware_board_with_bin_suffix_no_match(void)
{
    // A consumer that accidentally passes "taipanminer-tdongle-s3.bin"
    // (with the suffix) should NOT match because the parser appends another
    // ".bin", producing "taipanminer-tdongle-s3.bin.bin".
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_update_check_set_firmware_board("taipanminer-tdongle-s3.bin"));

    bb_http_client_set_mock_response(TDONGLE_BODY, strlen(TDONGLE_BODY), 200);
    bb_err_t err = bb_update_check_run_one();
    // Parser won't find "taipanminer-tdongle-s3.bin.bin" in the asset list.
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
}

// File-scope state for board_capture_parser (nested functions not portable).
static const char *s_captured_board = NULL;

static bb_err_t board_capture_parser(const char *body, size_t body_len,
                                     const char *board, char *out_tag, size_t tag_sz,
                                     char *out_url, size_t url_sz)
{
    (void)body; (void)body_len;
    s_captured_board = board;
    strncpy(out_tag, "v1.2.3", tag_sz - 1); out_tag[tag_sz - 1] = '\0';
    strncpy(out_url, "http://x/f.bin", url_sz - 1); out_url[url_sz - 1] = '\0';
    return BB_OK;
}

void test_bb_update_check_firmware_board_custom_parser_receives_board(void)
{
    // Custom-parser path: the board argument received by the parser must be
    // the overridden board name (not BOARD_NAME_FALLBACK).
    s_captured_board = NULL;
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_parser(board_capture_parser);
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_update_check_set_firmware_board("taipanminer-bitaxe-650"));

    bb_http_client_set_mock_response("x", 1, 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());
    TEST_ASSERT_NOT_NULL(s_captured_board);
    TEST_ASSERT_EQUAL_STRING("taipanminer-bitaxe-650", s_captured_board);
}

// ---------------------------------------------------------------------------
// Initial snapshot at init: ring has count=1 immediately after bb_update_check_init
// ---------------------------------------------------------------------------

void test_bb_update_check_init_alone_does_not_publish(void)
{
    // After init alone (without calling bb_update_check_publish_initial),
    // any ring attached to the topic must have count=0. This codifies the
    // new contract: callers must explicitly call publish_initial after attaching
    // the ring to populate the state topic.
    setenv("BB_EVENT_HOST_SYNC", "1", 1);
    reset_world();

    bb_event_topic_t topic = NULL;
    bb_err_t err = bb_event_topic_register("update.available", &topic);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_event_ring_t ring = NULL;
    err = bb_event_ring_attach_ex(topic, 4, 512, true, &ring);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // Init — does NOT post the initial snapshot anymore.
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_init(NULL));
    bb_event_pump(0);

    // Ring must have count=0 (no initial publish yet).
    TEST_ASSERT_EQUAL(0, (int)bb_event_ring_count(ring));

    bb_event_ring_detach(ring);
}

void test_bb_update_check_publish_initial_before_init_returns_invalid_state(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_update_check_publish_initial());
}

void test_bb_update_check_publish_initial_populates_ring(void)
{
    // After init and explicit bb_update_check_publish_initial, the ring
    // must have count=1 (the initial snapshot post).
    setenv("BB_EVENT_HOST_SYNC", "1", 1);
    reset_world();

    bb_event_topic_t topic = NULL;
    bb_err_t err = bb_event_topic_register("update.available", &topic);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_event_ring_t ring = NULL;
    err = bb_event_ring_attach_ex(topic, 4, 512, true, &ring);
    TEST_ASSERT_EQUAL(BB_OK, err);

    // Init — does not post yet.
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_init(NULL));
    bb_event_pump(0);
    TEST_ASSERT_EQUAL(0, (int)bb_event_ring_count(ring));

    // Now call publish_initial explicitly.
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_publish_initial());
    bb_event_pump(0);

    // Ring must now have count=1.
    TEST_ASSERT_EQUAL(1, (int)bb_event_ring_count(ring));

    bb_event_ring_detach(ring);
}

// File-scope state for the snapshot payload inspector.
static int    s_snap_count   = 0;
static char   s_snap_payload[512];
static size_t s_snap_size    = 0;

static void snap_capture(bb_event_topic_t topic, int32_t id,
                         const void *data, size_t size, void *user)
{
    (void)topic; (void)id; (void)user;
    s_snap_count++;
    s_snap_size = size;
    if (size > 0 && data && size < sizeof(s_snap_payload)) {
        memcpy(s_snap_payload, data, size);
        s_snap_payload[size] = '\0';
    }
}

void test_bb_update_check_publish_initial_snapshot_available_is_false(void)
{
    // The initial snapshot (from publish_initial) must have available=false
    // (no check has run yet).
    setenv("BB_EVENT_HOST_SYNC", "1", 1);
    reset_world();
    s_snap_count = 0;
    s_snap_size  = 0;
    s_snap_payload[0] = '\0';

    bb_event_topic_t topic = NULL;
    bb_event_topic_register("update.available", &topic);

    bb_event_ring_t ring = NULL;
    bb_event_ring_attach_ex(topic, 4, 512, true, &ring);

    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_init(NULL));
    // Publish the initial snapshot explicitly.
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_publish_initial());
    // Drain the event queue so the ring captures the post.
    bb_event_pump(0);

    // Subscribe with replay and inspect the payload.
    bb_event_sub_t sub = NULL;
    bb_event_ring_subscribe_with_replay(ring, snap_capture, NULL, &sub);

    TEST_ASSERT_EQUAL(1, s_snap_count);
    // The payload must contain "available":false
    TEST_ASSERT_NOT_NULL(strstr(s_snap_payload, "\"available\":false"));

    bb_event_unsubscribe(sub);
    bb_event_ring_detach(ring);
}

// ---------------------------------------------------------------------------
// bb_update_check_get_status: safe copy without holding lock externally
// ---------------------------------------------------------------------------

void test_bb_update_check_get_status_returns_copy_of_cached_state(void)
{
    // After a successful run, get_status must return the exact same fields
    // that were written by run_one (latest, download_url, available, last_check_ok).
    // Verifies the copy is complete and the caller does not need to hold any
    // lock externally.
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());

    bb_update_check_status_t a, b;
    // Two back-to-back calls without any intervening mutation must return equal state.
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_get_status(&a));
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_get_status(&b));

    TEST_ASSERT_EQUAL_STRING(a.latest,       b.latest);
    TEST_ASSERT_EQUAL_STRING(a.download_url, b.download_url);
    TEST_ASSERT_EQUAL(a.available,      b.available);
    TEST_ASSERT_EQUAL(a.last_check_ok,  b.last_check_ok);
    TEST_ASSERT_EQUAL(a.last_check_us,  b.last_check_us);

    // Mutating the returned copy must not affect the next call.
    a.available = false;
    a.latest[0] = '\0';
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_get_status(&b));
    TEST_ASSERT_TRUE(b.available);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", b.latest);
}

void test_bb_update_check_get_status_reflects_failure(void)
{
    // After a transport failure, get_status must report last_check_ok=false.
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);
    bb_update_check_run_one();

    bb_update_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_get_status(&st));
    TEST_ASSERT_FALSE(st.last_check_ok);
    TEST_ASSERT_FALSE(st.available);
}

// ---------------------------------------------------------------------------
// bb_update_check_kick
// ---------------------------------------------------------------------------

void test_bb_update_check_kick_returns_ok_on_host(void)
{
    // On host/Arduino backends, bb_update_check_kick() provides a synchronous
    // stub that calls bb_update_check_now(). This test verifies the host stub
    // returns BB_OK and performs the check without any worker task involvement.
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);

    // On host, kick() is synchronous and should drive a check.
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_kick());

    bb_update_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_get_status(&st));
    TEST_ASSERT_TRUE(st.available);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
}

// ---------------------------------------------------------------------------
// bb_nv_config_update_check_enabled runtime opt-out
// ---------------------------------------------------------------------------

void test_bb_update_check_status_enabled_is_true_by_default(void)
{
    // get_status must reflect enabled=true when bb_nv has not been changed.
    reset_world();
    bb_update_check_init(NULL);

    bb_update_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_get_status(&st));
    TEST_ASSERT_TRUE(st.enabled);
}

void test_bb_update_check_run_one_disabled_returns_ok_without_fetch(void)
{
    // When disabled via bb_nv, run_one must return BB_OK immediately and not
    // fetch — the mock response is intentionally absent to catch any attempt.
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_nv_config_set_update_check_enabled(false);

    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());

    // Status must be unchanged from the initial zero state.
    bb_update_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_get_status(&st));
    TEST_ASSERT_FALSE(st.last_check_ok);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_EQUAL(0, st.last_check_us);
    TEST_ASSERT_FALSE(st.enabled);
}

void test_bb_update_check_status_enabled_reflects_nv_flag(void)
{
    // get_status.enabled tracks bb_nv_config_update_check_enabled() live.
    reset_world();
    bb_update_check_init(NULL);

    bb_nv_config_set_update_check_enabled(false);
    bb_update_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_get_status(&st));
    TEST_ASSERT_FALSE(st.enabled);

    bb_nv_config_set_update_check_enabled(true);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_get_status(&st));
    TEST_ASSERT_TRUE(st.enabled);
}

void test_bb_update_check_reenabled_runs_check(void)
{
    // After disabling and re-enabling, run_one performs a real check.
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");

    // Disable: run_one must be a no-op.
    bb_nv_config_set_update_check_enabled(false);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());
    bb_update_check_status_t st;
    bb_update_check_get_status(&st);
    TEST_ASSERT_FALSE(st.last_check_ok);

    // Re-enable: subsequent run_one must fetch and succeed.
    bb_nv_config_set_update_check_enabled(true);
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_update_check_run_one());
    bb_update_check_get_status(&st);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_TRUE(st.available);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
}

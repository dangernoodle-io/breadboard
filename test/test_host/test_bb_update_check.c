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

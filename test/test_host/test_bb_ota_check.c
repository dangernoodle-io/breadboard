#include "unity.h"
#include "bb_ota_check.h"
#include "bb_ota_check_internal.h"
#include "bb_release_manifest.h"
#include "bb_http_client_host.h"
#include "bb_http_host.h"
#include "bb_ota_check_wire.h"
#include "bb_data.h"
#include "bb_json.h"
#include "bb_settings.h"
#include "bb_storage.h"
#include "fake_nvs_backend.h"
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

// Body with "unknown.bin" — matches BOARD_NAME_FALLBACK ("unknown").
static const char *UNKNOWN_BODY =
    "{\"tag_name\":\"v9.9.9\","
    "\"assets\":[{\"name\":\"unknown.bin\","
    "\"browser_download_url\":\"https://example.com/unknown.bin\"}]}";

static void reset_world(void)
{
    bb_storage_test_reset();
    fake_nvs_reset();
    bb_storage_register_backend("nvs", &s_fake_nvs_vtable, NULL);
    bb_ota_check_reset_for_test();
    bb_http_client_clear_mock();
    bb_data_test_reset();
    // B1-859: POST /api/update/config now drives bb_data_apply() against
    // the "ota_check_config" key -- bind it here (after the
    // bb_data_test_reset() above wipes the binding table) so every test
    // using this fixture, not just the config-post ones, can reach the
    // handler without an extra per-test bind call.
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_config_bind());
    g_mock_parser_calls = 0;
    g_pause_calls  = 0;
    g_resume_calls = 0;
    g_last_hook    = 0;
    g_pause_order  = 0;
    g_resume_order = 0;
    g_hook_seq     = 0;
    g_pause_returns = true;
}

// B1-1045: bb_data gather adapter wrapping bb_ota_check_gather() -- lets
// tests bind "update.available" and observe bb_data_touch() via
// bb_data_generation() instead of an event-ring replay.
static bb_err_t ota_check_gather_adapter(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    return bb_ota_check_gather((bb_ota_check_snap_t *)dst);
}

static void bind_ota_check_key(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&(bb_data_binding_t){
        .key = BB_OTA_CHECK_TOPIC, .desc = &bb_ota_check_wire_desc,
        .gather = ota_check_gather_adapter, .ctx = NULL }));
}

// ---------------------------------------------------------------------------
// init / accessors
// ---------------------------------------------------------------------------

void test_bb_ota_check_init_idempotent(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(NULL));
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(NULL));
}

void test_bb_ota_check_init_with_cfg_uses_overrides(void)
{
    reset_world();
    bb_ota_check_cfg_t cfg = { .interval_s = 60, .post_initial = true };
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(&cfg));
}

void test_bb_ota_check_is_initialized_reflects_init_state(void)
{
    reset_world();
    TEST_ASSERT_FALSE(bb_ota_check_is_initialized());
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(NULL));
    TEST_ASSERT_TRUE(bb_ota_check_is_initialized());
}

void test_bb_ota_check_get_status_before_init_returns_invalid_state(void)
{
    reset_world();
    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_ota_check_get_status(&st));
}

void test_bb_ota_check_get_status_null_out_returns_invalid_arg(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ota_check_get_status(NULL));
}

void test_bb_ota_check_set_releases_url_validates(void)
{
    reset_world();
    bb_ota_check_init(NULL);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ota_check_set_releases_url(NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ota_check_set_releases_url(""));

    char too_long[300];
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ota_check_set_releases_url(too_long));

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_set_releases_url("http://example.com/r.json"));
}

void test_bb_ota_check_set_releases_url_before_init_returns_invalid_state(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                      bb_ota_check_set_releases_url("http://example.com/r.json"));
}

void test_bb_ota_check_set_parser_before_init_returns_invalid_state(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_ota_check_set_parser(mock_parser));
}

void test_bb_ota_check_set_parser_null_restores_default(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_set_parser(mock_parser));
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_set_parser(NULL));
}

// ---------------------------------------------------------------------------
// run_one — transitions, sticky failure, post_initial
// ---------------------------------------------------------------------------

void test_bb_ota_check_run_one_before_init_returns_invalid_arg(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ota_check_run_one());
}

void test_bb_ota_check_run_one_without_url_returns_invalid_state(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_ota_check_run_one());
}

void test_bb_ota_check_now_without_url_returns_invalid_state(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_ota_check_now());
}

void test_bb_ota_check_now_before_init_returns_invalid_arg(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ota_check_now());
}

void test_bb_ota_check_run_one_newer_release_flips_available(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    /* Host bb_system_get_version returns "0.0.0" so v9.9.9 is newer. */
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_TRUE(st.available);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
    TEST_ASSERT_EQUAL_STRING("https://example.com/firmware.bin", st.download_url);
    TEST_ASSERT_NOT_EQUAL(0, st.last_check_us);
}

void test_bb_ota_check_run_one_same_version_keeps_unavailable(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    /* "0.0.0" matches host fallback running version; not newer. */
    char body[256];
    snprintf(body, sizeof(body), SAME_BODY_TEMPLATE, "v0.0.0");
    bb_http_client_set_mock_response(body, strlen(body), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_TRUE(st.last_check_ok);
}

void test_bb_ota_check_run_one_transport_failure_sticky(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);

    bb_err_t err = bb_ota_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_FALSE(st.last_check_ok);
    TEST_ASSERT_NOT_EQUAL(0, st.last_check_us);
}

void test_bb_ota_check_run_one_http_404_sticky_failure(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_response("Not Found", 9, 404);

    bb_err_t err = bb_ota_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_FALSE(st.last_check_ok);
}

void test_bb_ota_check_run_one_parse_failure_sticky(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_response("not json", 8, 200);

    bb_err_t err = bb_ota_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_FALSE(st.last_check_ok);
}

void test_bb_ota_check_run_one_recovers_after_failure(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");

    /* First: transport error. */
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);
    bb_ota_check_run_one();
    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_FALSE(st.last_check_ok);

    /* Then: successful response clears sticky. */
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    bb_ota_check_get_status(&st);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_TRUE(st.available);
}

void test_bb_ota_check_run_one_custom_parser_invoked(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_parser(mock_parser);
    bb_http_client_set_mock_response("anything", 8, 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    TEST_ASSERT_EQUAL(1, g_mock_parser_calls);

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
}

void test_bb_ota_check_post_initial_publishes_on_first_check(void)
{
    reset_world();
    bb_ota_check_cfg_t cfg = { .interval_s = 60, .post_initial = true };
    bb_ota_check_init(&cfg);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    /* Same-as-current body: no transition, but post_initial should publish. */
    char body[256];
    snprintf(body, sizeof(body), SAME_BODY_TEMPLATE, "v0.0.0");
    bb_http_client_set_mock_response(body, strlen(body), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_TRUE(st.last_check_ok);
}

void test_bb_ota_check_dev_tag_treated_as_older(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    /* "dev" tag is older than running "0.0.0" -> not available. */
    char body[256];
    snprintf(body, sizeof(body), SAME_BODY_TEMPLATE, "dev1.2.3");
    bb_http_client_set_mock_response(body, strlen(body), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_TRUE(st.last_check_ok);
}

void test_bb_ota_check_run_one_newer_to_same_transitions_back(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");

    /* First: newer release; available -> true. */
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_TRUE(st.available);

    /* Then: same-as-running tag; available -> false (transition). */
    char body[256];
    snprintf(body, sizeof(body), SAME_BODY_TEMPLATE, "v0.0.0");
    bb_http_client_set_mock_response(body, strlen(body), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    bb_ota_check_get_status(&st);
    TEST_ASSERT_FALSE(st.available);
}

void test_bb_ota_check_now_drives_a_check(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_now());

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_TRUE(st.available);
}

void test_bb_ota_check_custom_parser_transport_error(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_parser(mock_parser);
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);

    bb_err_t err = bb_ota_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
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

void test_bb_ota_check_custom_parser_parse_failure(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_parser(failing_parser);
    bb_http_client_set_mock_response("anything", 8, 200);

    bb_err_t err = bb_ota_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_FALSE(st.last_check_ok);
    TEST_ASSERT_NOT_EQUAL(0, st.last_check_us);
}

void test_bb_ota_check_custom_parser_http_404(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_parser(mock_parser);
    bb_http_client_set_mock_response("Not Found", 9, 404);

    bb_err_t err = bb_ota_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_FALSE(st.last_check_ok);
}

void test_bb_ota_check_custom_parser_body_exceeds_buf(void)
{
    // A response body larger than CUSTOM_PARSER_BUF_SIZE exercises the
    // buf_chunk_cb overflow path: copy==0 (false branch), overflow flag set
    // (true branch), and early-return on a subsequent chunk (overflow==true).
    // Also exercises the new overflow warning log path.
    // mock_parser ignores the body so the parse succeeds regardless.
    reset_world();
    bb_ota_check_cfg_t cfg = { .interval_s = 60, .post_initial = false };
    bb_ota_check_init(&cfg);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_parser(mock_parser);

    // 8705 bytes > 8192 (CUSTOM_PARSER_BUF_SIZE default) + 256-byte chunk padding.
    // Mock sends 256-byte chunks; 33rd chunk overflows the 8192-byte buffer,
    // 34th chunk hits the bc->overflow==true early-return path.
    const size_t big = 8705;
    char *body = (char *)malloc(big);
    TEST_ASSERT_NOT_NULL(body);
    memset(body, 'x', big - 1);
    body[big - 1] = '\0';
    bb_http_client_set_mock_response(body, big, 200);

    bb_err_t err = bb_ota_check_run_one();
    free(body);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
}

void test_bb_ota_check_custom_parser_post_initial_publishes(void)
{
    // Custom-parser path with post_initial=true and no version transition:
    // same_version_parser returns v0.0.0 == running, so transition=false but
    // initial_publish=true — exercises the `|| initial_publish` branch of the
    // `if (transition || initial_publish)` guard (L370-372).
    reset_world();
    bb_ota_check_cfg_t cfg = { .interval_s = 60, .post_initial = true };
    bb_ota_check_init(&cfg);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_parser(same_version_parser);
    bb_http_client_set_mock_response("x", 1, 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_TRUE(st.last_check_ok);

    // Second run: first_call=false, transition=false, initial_publish=false.
    // Exercises the `&&` short-circuit at L370 (first_call=false path) and
    // the `if (transition || initial_publish)` false branch at L371.
    bb_http_client_set_mock_response("x", 1, 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    bb_ota_check_get_status(&st);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_TRUE(st.last_check_ok);
}

// ---------------------------------------------------------------------------
// bb_ota_check_set_task_core (host stub — no-op, just exercise the symbol)
// ---------------------------------------------------------------------------

void test_bb_ota_check_set_task_core_host_is_noop(void)
{
    reset_world();
    bb_ota_check_set_task_core(0);
    bb_ota_check_set_task_core(1);
    bb_ota_check_set_task_core(-1);
    // No observable state to check on host — function returns void and host
    // stub has no worker task. Coverage-only test to keep the host stub line
    // executed.
}

// ---------------------------------------------------------------------------
// bb_ota_check_set_task_priority (host stub — no-op, just exercise the symbol)
// ---------------------------------------------------------------------------

void test_bb_ota_check_set_task_priority_host_is_noop(void)
{
    reset_world();
    bb_ota_check_set_task_priority(1);
    bb_ota_check_set_task_priority(21);
    bb_ota_check_set_task_priority(0);
    // No observable state — host stub has no worker task. Coverage-only test
    // to keep the host stub line executed.
}

// ---------------------------------------------------------------------------
// bb_ota_check_set_hooks
// ---------------------------------------------------------------------------

void test_bb_ota_check_set_hooks_before_init_stores_hooks(void)
{
    // Boot-strategy boards call set_hooks in app_main BEFORE bb_ota_check
    // is lazily initialized by bb_ota_boot. The store must succeed (BB_OK) and
    // the hooks must survive the later bb_ota_check_init (which does not
    // clear s_pause_hook/s_resume_hook) so run_one fires them.
    reset_world();
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_set_hooks(hook_pause, hook_resume));
    // Now init (as the lazy path would). Hooks must still be in place.
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(NULL));
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(1, g_resume_calls);
    TEST_ASSERT_TRUE(g_pause_order < g_resume_order);
}

void test_bb_ota_check_set_hooks_null_clears(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_set_hooks(hook_pause, hook_resume));
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_set_hooks(NULL, NULL));
    // After clearing, a successful run must not call either hook.
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    TEST_ASSERT_EQUAL(0, g_pause_calls);
    TEST_ASSERT_EQUAL(0, g_resume_calls);
}

void test_bb_ota_check_hooks_called_in_order_on_success(void)
{
    // Default (github streaming) parser path: pause before fetch, resume after.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    bb_ota_check_set_hooks(hook_pause, hook_resume);
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(1, g_resume_calls);
    // pause must have been called before resume
    TEST_ASSERT_TRUE(g_pause_order < g_resume_order);
    TEST_ASSERT_EQUAL(2, g_last_hook);
}

void test_bb_ota_check_hooks_resume_fires_on_transport_error(void)
{
    // Default path: resume must fire even when the fetch fails.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_hooks(hook_pause, hook_resume);
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);

    bb_err_t err = bb_ota_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);

    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(1, g_resume_calls);
    TEST_ASSERT_TRUE(g_pause_order < g_resume_order);
}

void test_bb_ota_check_hooks_resume_fires_on_parse_error(void)
{
    // Default path: resume must fire even when the parse step fails.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_hooks(hook_pause, hook_resume);
    bb_http_client_set_mock_response("not json", 8, 200);

    bb_err_t err = bb_ota_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);

    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(1, g_resume_calls);
    TEST_ASSERT_TRUE(g_pause_order < g_resume_order);
}

void test_bb_ota_check_hooks_called_once_per_run(void)
{
    // Two consecutive runs each trigger exactly one pause+resume pair.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    bb_ota_check_set_hooks(hook_pause, hook_resume);

    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(1, g_resume_calls);

    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    TEST_ASSERT_EQUAL(2, g_pause_calls);
    TEST_ASSERT_EQUAL(2, g_resume_calls);
}

void test_bb_ota_check_hooks_custom_parser_success(void)
{
    // Custom parser path: hooks must also fire.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_parser(mock_parser);
    bb_ota_check_set_hooks(hook_pause, hook_resume);
    bb_http_client_set_mock_response("anything", 8, 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(1, g_resume_calls);
    TEST_ASSERT_TRUE(g_pause_order < g_resume_order);
}

void test_bb_ota_check_hooks_custom_parser_transport_error(void)
{
    // Custom parser path: resume fires even on transport failure.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_parser(mock_parser);
    bb_ota_check_set_hooks(hook_pause, hook_resume);
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);

    bb_err_t err = bb_ota_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(1, g_resume_calls);
    TEST_ASSERT_TRUE(g_pause_order < g_resume_order);
}

void test_bb_ota_check_hooks_custom_parser_parse_error(void)
{
    // Custom parser path: resume fires even when parse fails.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_parser(failing_parser);
    bb_ota_check_set_hooks(hook_pause, hook_resume);
    bb_http_client_set_mock_response("anything", 8, 200);

    bb_err_t err = bb_ota_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(1, g_resume_calls);
    TEST_ASSERT_TRUE(g_pause_order < g_resume_order);
}

// pause returning false: fetch is skipped and resume is NOT called.
void test_bb_ota_check_pause_returns_false_skips_fetch(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_hooks(hook_pause, hook_resume);
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);

    g_pause_returns = false;
    bb_err_t err = bb_ota_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(0, g_resume_calls);
}

void test_bb_ota_check_pause_returns_false_custom_parser_skips_fetch(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_parser(mock_parser);
    bb_ota_check_set_hooks(hook_pause, hook_resume);
    bb_http_client_set_mock_response("anything", 8, 200);

    g_pause_returns = false;
    bb_err_t err = bb_ota_check_run_one();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    TEST_ASSERT_EQUAL(1, g_pause_calls);
    TEST_ASSERT_EQUAL(0, g_resume_calls);
}

// ---------------------------------------------------------------------------
// bb_ota_check_set_firmware_board
// ---------------------------------------------------------------------------

void test_bb_ota_check_set_firmware_board_before_init_returns_invalid_state(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE,
                      bb_ota_check_set_firmware_board("taipanminer-tdongle-s3"));
}

void test_bb_ota_check_set_firmware_board_too_long_returns_invalid_arg(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    // 64 chars is exactly BOARD_MAX — must be rejected (>= BOARD_MAX).
    char too_long[65];
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_ota_check_set_firmware_board(too_long));
}

void test_bb_ota_check_set_firmware_board_null_clears_to_default(void)
{
    // After setting a board and then passing NULL, a run with the default
    // unknown.bin body (BOARD_NAME_FALLBACK = "unknown") should match again.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_ota_check_set_firmware_board("taipanminer-tdongle-s3"));
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_set_firmware_board(NULL));

    bb_http_client_set_mock_response(UNKNOWN_BODY, strlen(UNKNOWN_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
}

void test_bb_ota_check_set_firmware_board_empty_string_clears_to_default(void)
{
    // Empty string "" reverts to BOARD_NAME_FALLBACK ("unknown"), same as NULL.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_ota_check_set_firmware_board("taipanminer-tdongle-s3"));
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_set_firmware_board(""));

    bb_http_client_set_mock_response(UNKNOWN_BODY, strlen(UNKNOWN_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
}

void test_bb_ota_check_firmware_board_matches_named_asset(void)
{
    // Setting "taipanminer-tdongle-s3" causes the parser to look for
    // "taipanminer-tdongle-s3.bin" — the TDONGLE_BODY has exactly that asset.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_ota_check_set_firmware_board("taipanminer-tdongle-s3"));

    bb_http_client_set_mock_response(TDONGLE_BODY, strlen(TDONGLE_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_TRUE(st.available);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
    TEST_ASSERT_EQUAL_STRING("https://example.com/taipanminer-tdongle-s3.bin",
                             st.download_url);
}

void test_bb_ota_check_firmware_board_default_does_not_match_named_asset(void)
{
    // Without setting a board, the default "unknown" fallback does NOT match
    // "taipanminer-tdongle-s3.bin". The release is parsed successfully (tag
    // found) but no matching asset exists: outcome=NO_ASSET, last_check_ok=true,
    // available=false. This is a SUCCESS terminal — callers polling last_check_ok
    // see true and do not hang/misreport.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    // Do NOT call set_firmware_board — use default ("unknown").

    bb_http_client_set_mock_response(TDONGLE_BODY, strlen(TDONGLE_BODY), 200);
    bb_err_t err = bb_ota_check_run_one();
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_EQUAL(BB_OTA_CHECK_OUTCOME_NO_ASSET, st.outcome);
    TEST_ASSERT_EQUAL_STRING("", st.download_url);
}

void test_bb_ota_check_firmware_board_with_bin_suffix_no_match(void)
{
    // A consumer that accidentally passes "taipanminer-tdongle-s3.bin"
    // (with the suffix) should NOT match because the parser appends another
    // ".bin", producing "taipanminer-tdongle-s3.bin.bin". The tag is found
    // but no matching asset exists → outcome=NO_ASSET, last_check_ok=true.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_ota_check_set_firmware_board("taipanminer-tdongle-s3.bin"));

    bb_http_client_set_mock_response(TDONGLE_BODY, strlen(TDONGLE_BODY), 200);
    bb_err_t err = bb_ota_check_run_one();
    // Parser won't find "taipanminer-tdongle-s3.bin.bin" in the asset list.
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_EQUAL(BB_OTA_CHECK_OUTCOME_NO_ASSET, st.outcome);
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

void test_bb_ota_check_firmware_board_custom_parser_receives_board(void)
{
    // Custom-parser path: the board argument received by the parser must be
    // the overridden board name (not BOARD_NAME_FALLBACK).
    s_captured_board = NULL;
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_parser(board_capture_parser);
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_ota_check_set_firmware_board("taipanminer-bitaxe-650"));

    bb_http_client_set_mock_response("x", 1, 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    TEST_ASSERT_NOT_NULL(s_captured_board);
    TEST_ASSERT_EQUAL_STRING("taipanminer-bitaxe-650", s_captured_board);
}

// ---------------------------------------------------------------------------
// bb_ota_check_get_status — board field
// ---------------------------------------------------------------------------

void test_bb_ota_check_get_status_board_reflects_fallback(void)
{
    // When no board is set, get_status.board is BOARD_NAME_FALLBACK ("unknown").
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_EQUAL_STRING("unknown", st.board);
}

void test_bb_ota_check_get_status_board_reflects_set_value(void)
{
    // When board is set, get_status.board mirrors the configured value.
    reset_world();
    bb_ota_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_ota_check_set_firmware_board("taipanminer-tdongle-s3"));
    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_EQUAL_STRING("taipanminer-tdongle-s3", st.board);
}

void test_bb_ota_check_get_status_board_reverts_to_fallback_after_clear(void)
{
    // After setting a board and clearing it (NULL), get_status.board is "unknown".
    reset_world();
    bb_ota_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_OK,
                      bb_ota_check_set_firmware_board("taipanminer-tdongle-s3"));
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_set_firmware_board(NULL));
    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_EQUAL_STRING("unknown", st.board);
}

// ---------------------------------------------------------------------------
// Initial snapshot at init: "update.available" generation bumps only on an
// explicit bb_ota_check_publish_initial() call (B1-1045: bb_event_ring
// replay-count assertions replaced by bb_data_generation() polling).
// ---------------------------------------------------------------------------

void test_bb_ota_check_init_alone_does_not_publish(void)
{
    // After init alone (without calling bb_ota_check_publish_initial), the
    // "update.available" bb_data generation must still be 0. This codifies
    // the contract: callers must explicitly call publish_initial to populate
    // the state key.
    reset_world();
    bind_ota_check_key();

    // Init — does NOT touch the generation anymore.
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(NULL));

    uint32_t gen = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_generation(BB_OTA_CHECK_TOPIC, &gen));
    TEST_ASSERT_EQUAL(0, (int)gen);
}

void test_bb_ota_check_publish_initial_before_init_returns_invalid_state(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_ota_check_publish_initial());
}

void test_bb_ota_check_publish_initial_bumps_generation(void)
{
    // After init and explicit bb_ota_check_publish_initial, the
    // "update.available" generation must have bumped by 1 (the initial
    // snapshot touch).
    reset_world();
    bind_ota_check_key();

    // Init — does not touch yet.
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(NULL));
    uint32_t gen_after_init = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_generation(BB_OTA_CHECK_TOPIC, &gen_after_init));
    TEST_ASSERT_EQUAL(0, (int)gen_after_init);

    // Now call publish_initial explicitly.
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_publish_initial());
    uint32_t gen_after_publish = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_generation(BB_OTA_CHECK_TOPIC, &gen_after_publish));

    // Generation must now have advanced by exactly 1.
    TEST_ASSERT_EQUAL(1, (int)gen_after_publish);
}

void test_bb_ota_check_publish_initial_snapshot_available_is_false(void)
{
    // The initial snapshot (from publish_initial) must have available=false
    // (no check has run yet). B1-1045: inspect the gathered snapshot struct
    // directly rather than a replayed SSE payload string.
    reset_world();
    bind_ota_check_key();

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(NULL));
    // Publish the initial snapshot explicitly.
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_publish_initial());

    uint32_t gen = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_generation(BB_OTA_CHECK_TOPIC, &gen));
    TEST_ASSERT_EQUAL(1, (int)gen);

    bb_ota_check_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_gather(&snap));
    TEST_ASSERT_FALSE(snap.available);
}

// ---------------------------------------------------------------------------
// bb_ota_check_get_status: safe copy without holding lock externally
// ---------------------------------------------------------------------------

void test_bb_ota_check_get_status_returns_copy_of_cached_state(void)
{
    // After a successful run, get_status must return the exact same fields
    // that were written by run_one (latest, download_url, available, last_check_ok).
    // Verifies the copy is complete and the caller does not need to hold any
    // lock externally.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    bb_ota_check_status_t a, b;
    // Two back-to-back calls without any intervening mutation must return equal state.
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&a));
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&b));

    TEST_ASSERT_EQUAL_STRING(a.latest,       b.latest);
    TEST_ASSERT_EQUAL_STRING(a.download_url, b.download_url);
    TEST_ASSERT_EQUAL(a.available,      b.available);
    TEST_ASSERT_EQUAL(a.last_check_ok,  b.last_check_ok);
    TEST_ASSERT_EQUAL(a.last_check_us,  b.last_check_us);

    // Mutating the returned copy must not affect the next call.
    a.available = false;
    a.latest[0] = '\0';
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&b));
    TEST_ASSERT_TRUE(b.available);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", b.latest);
}

void test_bb_ota_check_get_status_reflects_failure(void)
{
    // After a transport failure, get_status must report last_check_ok=false.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);
    bb_ota_check_run_one();

    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_FALSE(st.last_check_ok);
    TEST_ASSERT_FALSE(st.available);
}

// ---------------------------------------------------------------------------
// bb_ota_check_kick — on-demand model (B1-240)
// ---------------------------------------------------------------------------

void test_bb_ota_check_kick_returns_ok_on_host(void)
{
    // On host the kick() stub acquires the in-flight guard, runs the check
    // synchronously, then releases the guard.  Verify check executes and
    // status is updated.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_kick());

    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_TRUE(st.available);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
}

void test_bb_ota_check_kick_clears_in_flight_after_run(void)
{
    // After kick() completes the in-flight guard must be false so a
    // subsequent kick can proceed.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_kick());
    // Guard must be clear — second kick should succeed.
    TEST_ASSERT_FALSE(bb_ota_check_get_in_flight_for_test());
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_kick());
}

void test_bb_ota_check_kick_in_flight_is_noop(void)
{
    // If the in-flight guard is already set (simulating a concurrent spawn),
    // kick() must return BB_OK without performing a fetch and must not
    // clear the guard.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    // No mock response — if a fetch were attempted the mock would fail.

    // Inject the in-flight state.
    bb_ota_check_set_in_flight_for_test(true);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_kick());

    // Guard must still be set (kick did not clear it — the "running" task owns it).
    TEST_ASSERT_TRUE(bb_ota_check_get_in_flight_for_test());

    // Status must be unchanged (no fetch occurred).
    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_FALSE(st.last_check_ok);
    TEST_ASSERT_EQUAL(0, st.last_check_us);

    // Clean up the guard so subsequent tests start clean.
    bb_ota_check_set_in_flight_for_test(false);
}

void test_bb_ota_check_in_flight_resets_on_reset_world(void)
{
    // After reset_world() (which calls bb_ota_check_reset_for_test),
    // the in-flight guard must be false regardless of prior state.
    bb_ota_check_set_in_flight_for_test(true);
    reset_world();
    TEST_ASSERT_FALSE(bb_ota_check_get_in_flight_for_test());
}

// ---------------------------------------------------------------------------
// bb_settings_update_check_enabled_get runtime opt-out
// ---------------------------------------------------------------------------

void test_bb_ota_check_status_enabled_is_true_by_default(void)
{
    // get_status must reflect enabled=true when bb_nv has not been changed.
    reset_world();
    bb_ota_check_init(NULL);

    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_TRUE(st.enabled);
}

void test_bb_ota_check_run_one_disabled_returns_ok_without_fetch(void)
{
    // When disabled via bb_nv, run_one must return BB_OK immediately and not
    // fetch — the mock response is intentionally absent to catch any attempt.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_settings_update_check_enabled_set(false);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    // Status must be unchanged from the initial zero state.
    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_FALSE(st.last_check_ok);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_EQUAL(0, st.last_check_us);
    TEST_ASSERT_FALSE(st.enabled);
}

void test_bb_ota_check_status_enabled_reflects_nv_flag(void)
{
    // get_status.enabled tracks bb_settings_update_check_enabled_get() live.
    reset_world();
    bb_ota_check_init(NULL);

    bb_settings_update_check_enabled_set(false);
    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_FALSE(st.enabled);

    bb_settings_update_check_enabled_set(true);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_TRUE(st.enabled);
}

void test_bb_ota_check_reenabled_runs_check(void)
{
    // After disabling and re-enabling, run_one performs a real check.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");

    // Disable: run_one must be a no-op.
    bb_settings_update_check_enabled_set(false);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_FALSE(st.last_check_ok);

    // Re-enable: subsequent run_one must fetch and succeed.
    bb_settings_update_check_enabled_set(true);
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    bb_ota_check_get_status(&st);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_TRUE(st.available);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
}

// ---------------------------------------------------------------------------
// GET /api/update/config  — HTTP handler tests
// ---------------------------------------------------------------------------

void test_update_check_config_get_returns_enabled_true_by_default(void)
{
    reset_world();
    bb_settings_update_check_enabled_set(true);

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_ota_check_config_get_handler(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"enabled\":true"));
    bb_http_host_capture_free(&cap);
}

void test_update_check_config_get_returns_enabled_false_when_disabled(void)
{
    reset_world();
    bb_settings_update_check_enabled_set(false);

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_ota_check_config_get_handler(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"enabled\":false"));
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// POST /api/update/config — HTTP handler tests
// ---------------------------------------------------------------------------

void test_update_check_config_post_toggles_to_false(void)
{
    reset_world();
    bb_settings_update_check_enabled_set(true);

    const char *body = "{\"enabled\":false}";
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_req_body(body, (int)strlen(body));
    bb_err_t rc = bb_ota_check_config_post_handler(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"enabled\":false"));
    TEST_ASSERT_FALSE(bb_settings_update_check_enabled_get());
    bb_http_host_capture_free(&cap);
}

void test_update_check_config_post_toggles_to_true(void)
{
    reset_world();
    bb_settings_update_check_enabled_set(false);

    const char *body = "{\"enabled\":true}";
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_req_body(body, (int)strlen(body));
    bb_err_t rc = bb_ota_check_config_post_handler(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"enabled\":true"));
    TEST_ASSERT_TRUE(bb_settings_update_check_enabled_get());
    bb_http_host_capture_free(&cap);
}

void test_update_check_config_post_no_body_returns_400(void)
{
    reset_world();

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    // no body injected — body_len will be 0
    bb_err_t rc = bb_ota_check_config_post_handler(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(400, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_update_check_config_post_invalid_json_returns_400(void)
{
    reset_world();

    const char *body = "not-json{{{";
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_req_body(body, (int)strlen(body));
    bb_err_t rc = bb_ota_check_config_post_handler(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(400, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_update_check_config_post_missing_enabled_field_returns_400(void)
{
    reset_world();

    const char *body = "{\"other\":true}";
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_req_body(body, (int)strlen(body));
    bb_err_t rc = bb_ota_check_config_post_handler(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(400, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_update_check_config_post_wrong_type_for_enabled_returns_400(void)
{
    reset_world();

    const char *body = "{\"enabled\":\"yes\"}";
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_req_body(body, (int)strlen(body));
    bb_err_t rc = bb_ota_check_config_post_handler(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(400, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_update_check_config_post_truncated_body_returns_400(void)
{
    // Body scans as valid JSON up to the point it's cut off mid-object --
    // BB_ERR_PARSE_INCOMPLETE, not BB_ERR_PARSE_GRAMMAR (see
    // bb_serialize_json_scan.c). Regression coverage for the truncated-body
    // status-mapping bug: pre-#955 this aliased BB_ERR_INVALID_STATE and
    // fell through to the generic 500 branch; post-#955 it's the disjoint
    // BB_ERR_PARSE_INCOMPLETE code, which must still map to 400 (parity
    // with the pre-B1-859 bb_json handler's any-unparseable-body-is-400
    // behavior).
    reset_world();

    const char *body = "{\"enabled\":true";
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_req_body(body, (int)strlen(body));
    bb_err_t rc = bb_ota_check_config_post_handler(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(400, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_update_check_config_get_route_descriptor_is_correct(void)
{
    const bb_route_t *r = bb_ota_check_config_get_route();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(BB_HTTP_GET, r->method);
    TEST_ASSERT_EQUAL_STRING("/api/update/config", r->path);
    TEST_ASSERT_EQUAL_STRING("update", r->tag);
}

void test_update_check_config_post_route_descriptor_is_correct(void)
{
    const bb_route_t *r = bb_ota_check_config_post_route();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(BB_HTTP_POST, r->method);
    TEST_ASSERT_EQUAL_STRING("/api/update/config", r->path);
    TEST_ASSERT_EQUAL_STRING("update", r->tag);
    TEST_ASSERT_NOT_NULL(r->request_schema);
}

void test_update_check_config_post_oversized_body_returns_400(void)
{
    // body_len > BB_OTA_CHECK_CONFIG_BODY_MAX (64) — exercises branch 2
    // of the `body_len <= 0 || body_len > MAX` guard at line 487.
    reset_world();

    // 65 bytes of JSON-ish content — more than the 64-byte cap.
    const char *big_body = "{\"enabled\":true,\"extra\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}";
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_req_body(big_body, (int)strlen(big_body));
    bb_err_t rc = bb_ota_check_config_post_handler(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(400, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_update_check_config_post_recv_failure_returns_400(void)
{
    // bb_http_req_recv returns -1 — exercises the n < 0 branch at line 493.
    reset_world();

    const char *body = "{\"enabled\":true}";
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_req_body(body, (int)strlen(body));
    bb_http_host_force_recv_fail(true);
    bb_err_t rc = bb_ota_check_config_post_handler(req);
    bb_http_host_force_recv_fail(false);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(400, cap.status);
    bb_http_host_capture_free(&cap);
}

void test_update_check_config_post_nv_write_failure_returns_500(void)
{
    // bb_settings_update_check_enabled_set fails — exercises the err != BB_OK
    // branch at line 511 and the 500 response path.
    reset_world();
    fake_nvs_backend_fail_set_key("update_check_en");

    const char *body = "{\"enabled\":false}";
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_capture_set_req_body(body, (int)strlen(body));
    bb_err_t rc = bb_ota_check_config_post_handler(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(500, cap.status);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// B1-859: sentinel-default assertions -- drives bb_data_apply() directly
// against the "ota_check_config" key (bypassing the HTTP handler) to prove
// the PATCH-mode gather() seed is what makes an absent "enabled" field
// still reject, and that a present field overwrites the sentinel and is
// durably applied. See bb_ota_check_common.c's banner comment above
// config_gather()/config_apply() for the full sentinel rationale.
// ---------------------------------------------------------------------------

void test_ota_check_config_apply_missing_enabled_returns_validation_error(void)
{
    reset_world();
    bb_settings_update_check_enabled_set(true);

    const char *body = "{}";
    uint8_t dst_scratch = 0xAA;  // pre-poisoned; must be overwritten by gather()'s seed, not left as-is
    char    parse_scratch[3072];
    bb_data_apply_req_t req = {
        .fmt               = BB_FORMAT_JSON,
        .key               = "ota_check_config",
        .mode              = BB_DATA_APPLY_PATCH,
        .body              = body,
        .body_len          = strlen(body),
        .parse_scratch     = parse_scratch,
        .parse_scratch_cap = sizeof(parse_scratch),
        .dst_scratch       = &dst_scratch,
        .dst_scratch_cap   = sizeof(dst_scratch),
    };

    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, bb_data_apply(&req));
    // The NV flag must be untouched -- apply() rejected before ever calling
    // bb_settings_update_check_enabled_set().
    TEST_ASSERT_TRUE(bb_settings_update_check_enabled_get());
}

void test_ota_check_config_apply_present_enabled_overwrites_sentinel_and_persists(void)
{
    reset_world();
    bb_settings_update_check_enabled_set(true);

    const char *body = "{\"enabled\":false}";
    uint8_t dst_scratch = 0xAA;
    char    parse_scratch[3072];
    bb_data_apply_req_t req = {
        .fmt               = BB_FORMAT_JSON,
        .key               = "ota_check_config",
        .mode              = BB_DATA_APPLY_PATCH,
        .body              = body,
        .body_len          = strlen(body),
        .parse_scratch     = parse_scratch,
        .parse_scratch_cap = sizeof(parse_scratch),
        .dst_scratch       = &dst_scratch,
        .dst_scratch_cap   = sizeof(dst_scratch),
    };

    TEST_ASSERT_EQUAL(BB_OK, bb_data_apply(&req));
    TEST_ASSERT_FALSE(bb_settings_update_check_enabled_get());
}

// ---------------------------------------------------------------------------
// bb_ota_check_outcome_t — four terminal outcomes
// ---------------------------------------------------------------------------

// Body with tag_name but no matching asset for the board: no-asset terminal.
// Uses test-board name "test-board"; body has "other-board.bin" only.
static const char *NO_ASSET_BODY =
    "{\"tag_name\":\"v9.9.9\","
    "\"assets\":[{\"name\":\"other-board.bin\","
    "\"browser_download_url\":\"https://example.com/other-board.bin\"}]}";

void test_bb_ota_check_outcome_no_asset_streaming(void)
{
    // Streaming path (default GitHub parser): release parsed, no matching asset.
    // outcome=NO_ASSET, last_check_ok=true, available=false.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("test-board");
    bb_http_client_set_mock_response(NO_ASSET_BODY, strlen(NO_ASSET_BODY), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_EQUAL(BB_OTA_CHECK_OUTCOME_NO_ASSET, st.outcome);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_EQUAL_STRING("", st.download_url);
    TEST_ASSERT_EQUAL_STRING("", st.latest);
}

// Parser that returns BB_OK but leaves url empty (simulates no-asset for custom path).
static bb_err_t no_asset_custom_parser(const char *body, size_t body_len,
                                       const char *board, char *out_tag, size_t tag_sz,
                                       char *out_url, size_t url_sz)
{
    (void)body; (void)body_len; (void)board;
    strncpy(out_tag, "v9.9.9", tag_sz - 1);
    out_tag[tag_sz - 1] = '\0';
    out_url[0] = '\0';  // empty url = no-asset terminal
    return BB_OK;
}

void test_bb_ota_check_outcome_no_asset_custom_parser(void)
{
    // Custom parser path: parser returns BB_OK with empty url.
    // outcome=NO_ASSET, last_check_ok=true, available=false.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("test-board");
    bb_ota_check_set_parser(no_asset_custom_parser);
    bb_http_client_set_mock_response("anything", 8, 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_EQUAL(BB_OTA_CHECK_OUTCOME_NO_ASSET, st.outcome);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_EQUAL_STRING("", st.download_url);
}

// No-asset on first check with post_initial=true: exercises the
// initial_publish branch of the NO_ASSET path (bb_ota_check_common.c:380).
void test_bb_ota_check_outcome_no_asset_post_initial_streaming(void)
{
    reset_world();
    bb_ota_check_cfg_t cfg = { .interval_s = 60, .post_initial = true };
    bb_ota_check_init(&cfg);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");  // NO_ASSET_BODY lacks firmware.bin
    bb_http_client_set_mock_response(NO_ASSET_BODY, strlen(NO_ASSET_BODY), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_EQUAL(BB_OTA_CHECK_OUTCOME_NO_ASSET, st.outcome);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_FALSE(st.available);
}

// First check finds an update (available=true); second finds no asset.
// was_available=true -> transition branch of NO_ASSET path (common.c:379).
void test_bb_ota_check_outcome_no_asset_transition_streaming(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");

    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_TRUE(st.available);

    bb_http_client_set_mock_response(NO_ASSET_BODY, strlen(NO_ASSET_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    bb_ota_check_get_status(&st);
    TEST_ASSERT_EQUAL(BB_OTA_CHECK_OUTCOME_NO_ASSET, st.outcome);
    TEST_ASSERT_FALSE(st.available);
}

// Custom-parser counterpart of the post_initial no-asset case (common.c:486).
void test_bb_ota_check_outcome_no_asset_post_initial_custom(void)
{
    reset_world();
    bb_ota_check_cfg_t cfg = { .interval_s = 60, .post_initial = true };
    bb_ota_check_init(&cfg);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("test-board");
    bb_ota_check_set_parser(no_asset_custom_parser);
    bb_http_client_set_mock_response("anything", 8, 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_EQUAL(BB_OTA_CHECK_OUTCOME_NO_ASSET, st.outcome);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_FALSE(st.available);
}

// Custom parser: available on the first call, no-asset on the second.
// was_available=true -> transition branch of NO_ASSET path (common.c:485).
static int g_avail_then_noasset_calls = 0;
static bb_err_t avail_then_no_asset_parser(const char *body, size_t body_len,
                                           const char *board, char *out_tag, size_t tag_sz,
                                           char *out_url, size_t url_sz)
{
    (void)body; (void)body_len; (void)board;
    g_avail_then_noasset_calls++;
    strncpy(out_tag, "v9.9.9", tag_sz - 1);
    out_tag[tag_sz - 1] = '\0';
    if (g_avail_then_noasset_calls == 1) {
        strncpy(out_url, "https://example.com/firmware.bin", url_sz - 1);
        out_url[url_sz - 1] = '\0';
    } else {
        out_url[0] = '\0';  // no asset on the re-check
    }
    return BB_OK;
}

void test_bb_ota_check_outcome_no_asset_transition_custom(void)
{
    reset_world();
    g_avail_then_noasset_calls = 0;
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("test-board");
    bb_ota_check_set_parser(avail_then_no_asset_parser);
    bb_http_client_set_mock_response("anything", 8, 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    bb_ota_check_status_t st;
    bb_ota_check_get_status(&st);
    TEST_ASSERT_TRUE(st.available);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    bb_ota_check_get_status(&st);
    TEST_ASSERT_EQUAL(BB_OTA_CHECK_OUTCOME_NO_ASSET, st.outcome);
    TEST_ASSERT_FALSE(st.available);
}

void test_bb_ota_check_outcome_available_streaming(void)
{
    // Streaming path: newer version + matching asset -> outcome=AVAILABLE.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_EQUAL(BB_OTA_CHECK_OUTCOME_AVAILABLE, st.outcome);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_TRUE(st.available);
}

void test_bb_ota_check_outcome_up_to_date_streaming(void)
{
    // Streaming path: same version -> outcome=UP_TO_DATE.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    char body[256];
    snprintf(body, sizeof(body), SAME_BODY_TEMPLATE, "v0.0.0");
    bb_http_client_set_mock_response(body, strlen(body), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_EQUAL(BB_OTA_CHECK_OUTCOME_UP_TO_DATE, st.outcome);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_FALSE(st.available);
}

void test_bb_ota_check_outcome_failed_transport_error(void)
{
    // Transport error -> outcome=FAILED, last_check_ok=false.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);

    bb_ota_check_run_one();

    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_EQUAL(BB_OTA_CHECK_OUTCOME_FAILED, st.outcome);
    TEST_ASSERT_FALSE(st.last_check_ok);
}

void test_bb_ota_check_outcome_failed_parse_error(void)
{
    // Malformed body (no tag_name) -> outcome=FAILED, last_check_ok=false.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_response("not json", 8, 200);

    bb_ota_check_run_one();

    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_EQUAL(BB_OTA_CHECK_OUTCOME_FAILED, st.outcome);
    TEST_ASSERT_FALSE(st.last_check_ok);
}

void test_bb_ota_check_outcome_unknown_before_any_check(void)
{
    // Before any check runs, outcome=UNKNOWN.
    reset_world();
    bb_ota_check_init(NULL);

    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_EQUAL(BB_OTA_CHECK_OUTCOME_UNKNOWN, st.outcome);
}

void test_bb_ota_check_outcome_available_custom_parser(void)
{
    // Custom parser returning a newer version -> outcome=AVAILABLE.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_parser(mock_parser);
    bb_http_client_set_mock_response("anything", 8, 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_EQUAL(BB_OTA_CHECK_OUTCOME_AVAILABLE, st.outcome);
    TEST_ASSERT_TRUE(st.available);
}

void test_bb_ota_check_outcome_failed_custom_parser_error(void)
{
    // Custom parser returning an error -> outcome=FAILED.
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_parser(failing_parser);
    bb_http_client_set_mock_response("anything", 8, 200);

    bb_ota_check_run_one();

    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_EQUAL(BB_OTA_CHECK_OUTCOME_FAILED, st.outcome);
    TEST_ASSERT_FALSE(st.last_check_ok);
}

// ---------------------------------------------------------------------------
// bb_ota_check_run_blocking (host stub)
// ---------------------------------------------------------------------------

void test_bb_ota_check_run_blocking_before_init_returns_invalid_state(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_ota_check_run_blocking(5000));
}

void test_bb_ota_check_run_blocking_runs_check_on_host(void)
{
    // On host, run_blocking is synchronous and behaves like now().
    reset_world();
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_blocking(5000));

    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_TRUE(st.available);
    TEST_ASSERT_TRUE(st.last_check_ok);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", st.latest);
}

// ---------------------------------------------------------------------------
// bb_ota_check_mark_check_on_apply
// ---------------------------------------------------------------------------

void test_bb_ota_check_mark_check_on_apply_sets_status(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_mark_check_on_apply());

    bb_ota_check_status_t st;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_get_status(&st));
    TEST_ASSERT_EQUAL(BB_OTA_CHECK_OUTCOME_CHECK_ON_APPLY, st.outcome);
    TEST_ASSERT_FALSE(st.available);
    TEST_ASSERT_FALSE(st.last_check_ok);
}

void test_bb_ota_check_mark_check_on_apply_before_init_returns_invalid_state(void)
{
    reset_world();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_ota_check_mark_check_on_apply());
}

// ---------------------------------------------------------------------------
// B1-351: retained snapshot always current after any successful check
// ---------------------------------------------------------------------------

void test_bb_ota_check_retained_snapshot_current_after_available_check(void)
{
    // Streaming path, available=true. After run_one, gather the snapshot
    // struct directly and verify available=true, latest="v9.9.9" (B1-1045:
    // replaces the bb_event_ring replay-payload inspection).
    reset_world();
    bind_ota_check_key();

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(NULL));
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_publish_initial());

    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    uint32_t gen = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_generation(BB_OTA_CHECK_TOPIC, &gen));
    TEST_ASSERT_TRUE(gen >= 1);

    bb_ota_check_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_gather(&snap));
    TEST_ASSERT_TRUE(snap.available);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", snap.latest);
}

void test_bb_ota_check_retained_snapshot_current_after_repeated_check(void)
{
    // Streaming path, available=true on first check. Second check with same
    // response — retained snapshot must still have available=true and the
    // generation must grow (unconditional touch fires on every successful
    // check).
    reset_world();
    bind_ota_check_key();

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(NULL));
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_publish_initial());

    // First check.
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    uint32_t gen_after_first = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_generation(BB_OTA_CHECK_TOPIC, &gen_after_first));

    // Second check with same response — no transition, but must still touch.
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    uint32_t gen_after_second = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_generation(BB_OTA_CHECK_TOPIC, &gen_after_second));

    TEST_ASSERT_TRUE(gen_after_second > gen_after_first);

    bb_ota_check_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_gather(&snap));
    TEST_ASSERT_TRUE(snap.available);
}

void test_bb_ota_check_retained_snapshot_current_custom_parser_available(void)
{
    // Custom parser path, available=true.
    reset_world();
    bind_ota_check_key();

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(NULL));
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_parser(mock_parser);
    bb_http_client_set_mock_response("anything", 8, 200);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_publish_initial());

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());

    uint32_t gen = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_generation(BB_OTA_CHECK_TOPIC, &gen));
    TEST_ASSERT_TRUE(gen >= 1);

    bb_ota_check_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_gather(&snap));
    TEST_ASSERT_TRUE(snap.available);
    TEST_ASSERT_EQUAL_STRING("v9.9.9", snap.latest);
}

void test_bb_ota_check_retained_snapshot_current_after_repeated_custom_parser_check(void)
{
    // Custom parser path. Second run_one also touches (generation grows).
    reset_world();
    bind_ota_check_key();

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(NULL));
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_parser(mock_parser);

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_publish_initial());

    // First check.
    bb_http_client_set_mock_response("anything", 8, 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    uint32_t gen_after_first = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_generation(BB_OTA_CHECK_TOPIC, &gen_after_first));

    // Second check — no transition, but must still touch.
    bb_http_client_set_mock_response("anything", 8, 200);
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_run_one());
    uint32_t gen_after_second = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_data_generation(BB_OTA_CHECK_TOPIC, &gen_after_second));

    TEST_ASSERT_TRUE(gen_after_second > gen_after_first);

    // Verify the gathered snapshot is still available=true.
    bb_ota_check_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_gather(&snap));
    TEST_ASSERT_TRUE(snap.available);
}

// ---------------------------------------------------------------------------
// OTA claim arbiter tests
// ---------------------------------------------------------------------------

void test_bb_ota_check_ota_claim_acquire_free_returns_ok(void)
{
    reset_world();
    bb_ota_check_ota_claim_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_ota_claim_acquire("ota_pull"));
    TEST_ASSERT_EQUAL_STRING("ota_pull", bb_ota_check_ota_claim_holder_for_test());
    bb_ota_check_ota_claim_release("ota_pull");
}

void test_bb_ota_check_ota_claim_acquire_conflict_returns_err(void)
{
    reset_world();
    bb_ota_check_ota_claim_reset();
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_ota_claim_acquire("ota_pull"));
    // Different id → conflict.
    TEST_ASSERT_NOT_EQUAL(BB_OK, bb_ota_check_ota_claim_acquire("upd_check"));
    // Original holder unchanged.
    TEST_ASSERT_EQUAL_STRING("ota_pull", bb_ota_check_ota_claim_holder_for_test());
    bb_ota_check_ota_claim_release("ota_pull");
}

void test_bb_ota_check_ota_claim_release_frees_slot(void)
{
    reset_world();
    bb_ota_check_ota_claim_reset();
    bb_ota_check_ota_claim_acquire("ota_pull");
    bb_ota_check_ota_claim_release("ota_pull");
    // Slot free — second acquirer wins.
    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_ota_claim_acquire("upd_check"));
    bb_ota_check_ota_claim_release("upd_check");
}

// B1-461: guard the shared /api/update/* route-path constants against
// accidental drift. These strings are the externally-consumed HTTP
// contract (taipan-cli, webui) — byte-identical to their pre-refactor
// hand-typed literals.
void test_bb_ota_check_route_consts_match_legacy_literals(void)
{
    TEST_ASSERT_EQUAL_STRING("/api/update/apply", BB_ROUTE_UPDATE_APPLY);
    TEST_ASSERT_EQUAL_STRING("/api/update/check", BB_ROUTE_UPDATE_CHECK);
    TEST_ASSERT_EQUAL_STRING("/api/update/progress", BB_ROUTE_UPDATE_PROGRESS);
    TEST_ASSERT_EQUAL_STRING("/api/update/status", BB_ROUTE_UPDATE_STATUS);
}

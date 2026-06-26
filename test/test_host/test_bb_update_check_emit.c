// Host tests for bb_update_check_emit_status_json — the shared JSON emitter
// factored out of the persistent /api/update/status route handler.
// Uses the bb_http_host_capture_* harness (same pattern as the config-handler
// tests in test_bb_update_check.c).
#include "unity.h"
#include "bb_update_check.h"
#include "bb_update_check_internal.h"
#include "bb_http_host.h"
#include "bb_http_client_host.h"
#include "bb_event.h"
#include "bb_event_test.h"
#include "bb_nv.h"
#include <string.h>

// TA-462: verify the version buffer is big enough for the longest dev format.
// "dev-<7sha>+<4hash>-bb-<7sha>+<4hash>" is 36 chars; BB_UPDATE_CHECK_VERSION_BUF
// must be > 36 to prevent truncation of /api/update/status `current` field.
_Static_assert(BB_UPDATE_CHECK_VERSION_BUF > 36,
    "BB_UPDATE_CHECK_VERSION_BUF too small for longest dev version format");

// Known good body — v9.9.9 is newer than the host running version "0.0.0".
static const char *VALID_BODY =
    "{\"tag_name\":\"v9.9.9\","
    "\"assets\":[{\"name\":\"firmware.bin\","
    "\"browser_download_url\":\"https://example.com/firmware.bin\"}]}";

static void reset_world(void)
{
    bb_update_check_reset_for_test();
    bb_http_client_clear_mock();
    bb_event_reset_for_test();
    bb_event_init(NULL);
}

// ---------------------------------------------------------------------------
// emit before init — 503 + error field
// ---------------------------------------------------------------------------

void test_emit_status_json_before_init_returns_503(void)
{
    reset_world();
    // no bb_update_check_init call — should emit 503.
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_update_check_emit_status_json(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(503, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"error\""));
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// emit after init, before any check — initial zero-state fields present
// ---------------------------------------------------------------------------

void test_emit_status_json_after_init_has_all_required_fields(void)
{
    reset_world();
    bb_update_check_init(NULL);

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_update_check_emit_status_json(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    // All required fields must be present.
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"current\""));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"latest\""));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"download_url\""));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"available\""));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"last_check_ok\""));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"enabled\""));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"outcome\""));
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// emit after a successful check — available=true, correct outcome
// ---------------------------------------------------------------------------

void test_emit_status_json_available_true_after_newer_release(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_firmware_board("firmware");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    bb_update_check_now();

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_update_check_emit_status_json(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"available\":true"));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"last_check_ok\":true"));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"outcome\":\"available\""));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"latest\":\"v9.9.9\""));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"last_check_ts\""));
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// emit after a failed check — available=false, outcome=check_failed
// ---------------------------------------------------------------------------

void test_emit_status_json_outcome_check_failed(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);
    bb_update_check_now();

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_update_check_emit_status_json(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"available\":false"));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"last_check_ok\":false"));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"outcome\":\"check_failed\""));
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// emit reflects outcome=unknown before any check
// ---------------------------------------------------------------------------

void test_emit_status_json_outcome_unknown_before_check(void)
{
    reset_world();
    bb_update_check_init(NULL);

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_update_check_emit_status_json(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"outcome\":\"unknown\""));
    // last_check_ts must be absent when no check has run (last_check_us == 0).
    TEST_ASSERT_NULL(strstr(cap.body, "\"last_check_ts\""));
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// emit reflects outcome=up_to_date when the release is not newer than current
// ---------------------------------------------------------------------------

void test_emit_status_json_outcome_up_to_date(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_firmware_board("firmware");
    // tag == host running version (0.0.0) → not newer → up_to_date.
    static const char *SAME_BODY =
        "{\"tag_name\":\"v0.0.0\","
        "\"assets\":[{\"name\":\"firmware.bin\","
        "\"browser_download_url\":\"https://example.com/firmware.bin\"}]}";
    bb_http_client_set_mock_response(SAME_BODY, strlen(SAME_BODY), 200);
    bb_update_check_now();

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_update_check_emit_status_json(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"available\":false"));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"outcome\":\"up_to_date\""));
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// emit reflects outcome=no_asset when a newer release has no matching asset
// ---------------------------------------------------------------------------

void test_emit_status_json_outcome_no_asset(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_set_releases_url("http://example.com/r.json");
    bb_update_check_set_firmware_board("firmware");
    // newer tag (v9.9.9) but the only asset name doesn't match "firmware.bin".
    static const char *NOASSET_BODY =
        "{\"tag_name\":\"v9.9.9\","
        "\"assets\":[{\"name\":\"other-board.bin\","
        "\"browser_download_url\":\"https://example.com/other.bin\"}]}";
    bb_http_client_set_mock_response(NOASSET_BODY, strlen(NOASSET_BODY), 200);
    bb_update_check_now();

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_update_check_emit_status_json(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"outcome\":\"no_asset\""));
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// emit CORS headers are set
// ---------------------------------------------------------------------------

void test_emit_status_json_sets_cors_headers(void)
{
    reset_world();
    bb_update_check_init(NULL);

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_update_check_emit_status_json(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_TRUE(cap.has_acao);
    TEST_ASSERT_TRUE(cap.has_acapn);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// emit enabled field reflects nv flag
// ---------------------------------------------------------------------------

void test_emit_status_json_enabled_reflects_nv_flag(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_nv_config_set_update_check_enabled(false);

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_update_check_emit_status_json(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"enabled\":false"));
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// emit after mark_check_on_apply — outcome=check_on_apply, available=false
// ---------------------------------------------------------------------------

void test_emit_status_check_on_apply(void)
{
    reset_world();
    bb_update_check_init(NULL);
    bb_update_check_mark_check_on_apply();

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_update_check_emit_status_json(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"outcome\":\"check_on_apply\""));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"available\":false"));
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// emit when bb_http_resp_json_obj_begin fails — propagates the error
// ---------------------------------------------------------------------------

void test_emit_status_json_obj_begin_fail_returns_err(void)
{
    reset_world();
    bb_update_check_init(NULL);  /* ensures get_status returns BB_OK */

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_force_set_type_fail(true);
    bb_err_t rc = bb_update_check_emit_status_json(req);
    bb_http_host_force_set_type_fail(false);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
    bb_http_host_capture_free(&cap);
}

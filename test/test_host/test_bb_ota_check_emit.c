// Host tests for bb_ota_check_emit_status_json — the shared JSON emitter
// factored out of the persistent /api/update/status route handler.
// Uses the bb_http_host_capture_* harness (same pattern as the config-handler
// tests in test_bb_ota_check.c).
#include "unity.h"
#include "bb_data.h"
#include "bb_ota_check.h"
#include "bb_ota_check_internal.h"
#include "bb_http_host.h"
#include "bb_http_client_host.h"
#include "bb_serialize.h"
#include "bb_settings.h"
#include "bb_storage.h"
#include "fake_nvs_backend.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// TA-462: verify the version buffer is big enough for the longest dev format.
// "dev-<7sha>+<4hash>-bb-<7sha>+<4hash>" is 36 chars; BB_OTA_CHECK_VERSION_BUF
// must be > 36 to prevent truncation of /api/update/status `current` field.
_Static_assert(BB_OTA_CHECK_VERSION_BUF > 36,
    "BB_OTA_CHECK_VERSION_BUF too small for longest dev version format");

// Known good body — v9.9.9 is newer than the host running version "0.0.0".
static const char *VALID_BODY =
    "{\"tag_name\":\"v9.9.9\","
    "\"assets\":[{\"name\":\"firmware.bin\","
    "\"browser_download_url\":\"https://example.com/firmware.bin\"}]}";

static void reset_world(void)
{
    bb_storage_test_reset();
    fake_nvs_reset();
    bb_storage_register_backend("nvs", &s_fake_nvs_vtable, NULL);
    bb_ota_check_reset_for_test();
    bb_http_client_clear_mock();
}

// ---------------------------------------------------------------------------
// emit before init — 503 + error field
// ---------------------------------------------------------------------------

void test_emit_status_json_before_init_returns_503(void)
{
    reset_world();
    // no bb_ota_check_init call — should emit 503.
    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_ota_check_emit_status_json(req);
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
    bb_ota_check_init(NULL);

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_ota_check_emit_status_json(req);
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
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    bb_http_client_set_mock_response(VALID_BODY, strlen(VALID_BODY), 200);
    bb_ota_check_now();

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_ota_check_emit_status_json(req);
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
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);
    bb_ota_check_now();

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_ota_check_emit_status_json(req);
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
    bb_ota_check_init(NULL);

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_ota_check_emit_status_json(req);
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
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    // tag == host running version (0.0.0) → not newer → up_to_date.
    static const char *SAME_BODY =
        "{\"tag_name\":\"v0.0.0\","
        "\"assets\":[{\"name\":\"firmware.bin\","
        "\"browser_download_url\":\"https://example.com/firmware.bin\"}]}";
    bb_http_client_set_mock_response(SAME_BODY, strlen(SAME_BODY), 200);
    bb_ota_check_now();

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_ota_check_emit_status_json(req);
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
    bb_ota_check_init(NULL);
    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");
    // newer tag (v9.9.9) but the only asset name doesn't match "firmware.bin".
    static const char *NOASSET_BODY =
        "{\"tag_name\":\"v9.9.9\","
        "\"assets\":[{\"name\":\"other-board.bin\","
        "\"browser_download_url\":\"https://example.com/other.bin\"}]}";
    bb_http_client_set_mock_response(NOASSET_BODY, strlen(NOASSET_BODY), 200);
    bb_ota_check_now();

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_ota_check_emit_status_json(req);
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
    bb_ota_check_init(NULL);

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_ota_check_emit_status_json(req);
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
    bb_ota_check_init(NULL);
    bb_settings_update_check_enabled_set(false);

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_ota_check_emit_status_json(req);
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
    bb_ota_check_init(NULL);
    bb_ota_check_mark_check_on_apply();

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_ota_check_emit_status_json(req);
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
// BB_OTA_CHECK_OUTCOME_ENUM_JSON (bb_ota_check.h) must stay in sync with
// outcome_str() (bb_ota_check_common.c) — B1-462a hoist. Both /api/update/
// status schema sites (bb_ota_check_espidf.c, bb_ota_boot.c) reference this
// one constant so they cannot drift independently.
// ---------------------------------------------------------------------------

void test_update_outcome_enum_json_matches_outcome_str_values(void)
{
    static const char expected[] =
        "\"unknown\",\"up_to_date\",\"available\","
        "\"no_asset\",\"check_failed\",\"check_on_apply\"";
    TEST_ASSERT_EQUAL_STRING(expected, BB_OTA_CHECK_OUTCOME_ENUM_JSON);
}

// ---------------------------------------------------------------------------
// emit when bb_http_resp_json_obj_begin fails — propagates the error
// ---------------------------------------------------------------------------

void test_emit_status_json_obj_begin_fail_returns_err(void)
{
    reset_world();
    bb_ota_check_init(NULL);  /* ensures get_status returns BB_OK */

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_force_set_type_fail(true);
    bb_err_t rc = bb_ota_check_emit_status_json(req);
    bb_http_host_force_set_type_fail(false);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// B1-1053 PR3 -- REST GET /api/update/status cut over onto bb_data_render()
// against the "update.available" bb_data binding (bb_ota_check_bind(),
// bb_ota_check_wire.c), replacing the retired bb_json bb_cache serializer
// (bb_ota_check_serialize()). New coverage below: the {"ts_ms":N,"data":{...}}
// envelope shape, and the new `if (err != BB_OK) return err;` guard after
// bb_data_render() -- the ONE new error-propagation branch this cutover
// introduces (mirrors bb_diag_boot_render_envelope()'s own NOT_FOUND
// coverage, test_bb_diag_boot_render.c).
// ---------------------------------------------------------------------------

// ts_ms: "{"ts_ms":" followed by one or more digits, then ",\"data\":{...".
// Not deterministic (no bb_clock host test hook exists here either), so
// asserted STRUCTURALLY like test_diag_boot_render_envelope_shape().
void test_emit_status_json_envelope_shape(void)
{
    reset_world();
    bb_ota_check_init(NULL);

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_ota_check_emit_status_json(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);

    TEST_ASSERT_EQUAL_INT(0, strncmp(cap.body, "{\"ts_ms\":", 9));
    const char *p = cap.body + 9;
    TEST_ASSERT_TRUE(*p >= '0' && *p <= '9');
    while (*p >= '0' && *p <= '9') p++;
    TEST_ASSERT_EQUAL_INT(0, strncmp(p, ",\"data\":{", 9));
    // "data" carries exactly the pre-cutover root-level fields, now nested.
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"data\":{\"current\""));
    bb_http_host_capture_free(&cap);
}

// bb_data_render() resolves the "update.available" binding FIRST -- wiping
// the bb_data table AFTER bb_ota_check_init() has already bound it (via
// bb_ota_check_bind(), called from init) reproduces an unbound key without
// touching bb_ota_check's own s_initialized flag, exercising the new
// `if (err != BB_OK) return err;` guard right after bb_data_render() in
// bb_ota_check_emit_status_json().
void test_emit_status_json_render_unbound_key_propagates_not_found_as_500(void)
{
    reset_world();
    bb_ota_check_init(NULL);
    bb_data_test_reset();  // wipes the "update.available" binding bb_ota_check_init() just made

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_ota_check_emit_status_json(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, rc);
    TEST_ASSERT_EQUAL(500, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"error\":\"render failed\""));
    bb_http_host_capture_free(&cap);
}

// Worst-case render size proof for BB_OTA_CHECK_RENDER_BUF_BYTES (see its
// own doc comment, bb_ota_check_common.c) -- drives the ACTUAL emit path
// (bb_ota_check_now() -> bb_ota_check_mark_check_on_apply() ->
// bb_ota_check_emit_status_json()) with every wire field pushed to its
// genuine worst case:
//   - current/latest: 23 content chars -- the widest either can hold
//     (bb_ota_check_snap_t.current/latest are char[24], and bb_strlcpy
//     always NUL-terminates, so 23 chars is the true max, not the 24-byte
//     buffer size). current has no public setter (it mirrors the running
//     firmware version, fixed at init time) so
//     bb_ota_check_set_current_for_test() injects it directly; latest is 23
//     chars via the mock manifest's "tag_name".
//   - download_url: 255 content chars (char[256], same NUL-terminated-max
//     reasoning), densely populated with alternating '"'/'\\' -- the actual
//     worst-case byte-doubling input for bb_json_escape_write(). This field
//     is externally sourced (the GitHub release manifest fetched via
//     bb_ota_check_set_releases_url()), and bb_release_manifest_json_sink.c
//     dequotes JSON escapes on ingest, so the mock HTTP body below carries
//     each '"'/'\\' JSON-escaped (\" / \\) so the parsed, stored
//     download_url ends up with the raw quote/backslash characters.
//   - outcome: "check_on_apply" (14 chars) -- the longest of outcome_str()'s
//     literals -- via bb_ota_check_mark_check_on_apply(), which also forces
//     available=false and last_check_ok=false (both render as "false", the
//     wider of the two bool literals).
//   - ts / last_check_ts: driven toward their full int64 width via
//     bb_ota_check_set_ts_for_test(INT64_MIN, INT64_MIN). ts renders
//     directly as INT64_MIN ("-9223372036854775808", 20 chars); last_check_ts
//     is derived as last_check_us/1000000 (truncating integer division), so
//     it renders narrower in practice -- the render buffer is still sized
//     off the field's int64 TYPE bound, not this call site's current
//     arithmetic, so this remains a genuine (if not exactly tightest)
//     stress of that field.
void test_emit_status_json_render_buf_headroom(void)
{
    reset_world();
    bb_ota_check_init(NULL);

    // 23-char current, injected directly (no public setter -- see above).
    char current_23[24];
    memset(current_23, '9', sizeof(current_23) - 1);
    current_23[sizeof(current_23) - 1] = '\0';
    bb_ota_check_set_current_for_test(current_23);

    bb_ota_check_set_releases_url("http://example.com/r.json");
    bb_ota_check_set_firmware_board("firmware");

    // 23-char "latest" (tag_name in the mock manifest body).
    char tag_23[24];
    memset(tag_23, '8', sizeof(tag_23) - 1);
    tag_23[sizeof(tag_23) - 1] = '\0';

    // 255-char raw download_url content: alternating '"' and '\\'.
    char raw_url[256];
    for (size_t i = 0; i < sizeof(raw_url) - 1; i++) {
        raw_url[i] = (i % 2 == 0) ? '"' : '\\';
    }
    raw_url[sizeof(raw_url) - 1] = '\0';

    // JSON-escaped form of raw_url for embedding in the mock manifest body
    // -- bb_release_manifest_json_sink.c's underlying scanner dequotes this
    // back to raw_url on ingest.
    char esc_url[sizeof(raw_url) * 2];
    size_t ei = 0;
    for (size_t i = 0; i < sizeof(raw_url) - 1; i++) {
        esc_url[ei++] = '\\';
        esc_url[ei++] = raw_url[i];
    }
    esc_url[ei] = '\0';

    char body[1024];
    int n = snprintf(body, sizeof(body),
        "{\"tag_name\":\"%s\","
        "\"assets\":[{\"name\":\"firmware.bin\","
        "\"browser_download_url\":\"%s\"}]}", tag_23, esc_url);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(body));
    bb_http_client_set_mock_response(body, strlen(body), 200);
    bb_ota_check_now();

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_mark_check_on_apply());
    bb_ota_check_set_ts_for_test(INT64_MIN, INT64_MIN);

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_ota_check_emit_status_json(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"outcome\":\"check_on_apply\""));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"last_check_ts\""));

    // Measure the actual rendered "data" object -- everything from the '{'
    // right after "data": through to (but excluding) the outer object's own
    // closing '}' (the last byte of the whole document, since "data" is the
    // final field bb_ota_check_emit_status_json() writes).
    const char *data_key = strstr(cap.body, "\"data\":");
    TEST_ASSERT_NOT_NULL(data_key);
    const char *data_start = data_key + strlen("\"data\":");
    size_t total_len = strlen(cap.body);
    size_t data_len = (size_t)((cap.body + total_len - 1) - data_start);
    // 1024 == BB_OTA_CHECK_RENDER_BUF_BYTES (private to bb_ota_check_common.c,
    // not exposed to this TU) -- keep in sync with that #define.
    // Actual measured worst-case render: 739 bytes (verified via a direct
    // run of this test) -- well under the 1024-byte buffer; see
    // BB_OTA_CHECK_RENDER_BUF_BYTES's doc comment for the field-by-field
    // derivation and margin.
    TEST_ASSERT_TRUE(data_len < 1024);

    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// bb_ota_check_init()'s own bb_ota_check_bind() call site -- a bind failure
// (BB_DATA_MAX_BINDINGS already exhausted by other composed keys) is
// non-fatal to init() (mirrors bb_diag_boot_bind()'s call site,
// bb_diag_routes.c) but leaves "update.available" unbound, so a subsequent
// GET still surfaces via emit_status_json's own BB_ERR_NOT_FOUND/500 path
// (test_emit_status_json_render_unbound_key_propagates_not_found_as_500
// above covers that half already unbound-by-reset; this test covers the
// init()-time bind failure itself).
// ---------------------------------------------------------------------------

typedef struct { int64_t n; } dummy_snap_t;

static const bb_serialize_field_t s_dummy_fields[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(dummy_snap_t, n) },
};

static const bb_serialize_desc_t s_dummy_desc = {
    .type_name = "dummy_snap_t",
    .fields    = s_dummy_fields,
    .n_fields  = 1,
    .snap_size = sizeof(dummy_snap_t),
};

static bb_err_t dummy_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    ((dummy_snap_t *)dst)->n = 0;
    return BB_OK;
}

void test_ota_check_init_bind_failure_is_non_fatal_and_leaves_key_unbound(void)
{
    reset_world();
    bb_data_test_reset();

    char keys[BB_DATA_MAX_BINDINGS][32];
    for (int i = 0; i < BB_DATA_MAX_BINDINGS; i++) {
        snprintf(keys[i], sizeof(keys[i]), "dummy.%d", i);
        bb_data_binding_t b = { .key = keys[i], .desc = &s_dummy_desc, .gather = dummy_gather };
        TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));
    }

    TEST_ASSERT_EQUAL(BB_OK, bb_ota_check_init(NULL));

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t rc = bb_ota_check_emit_status_json(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, rc);
    TEST_ASSERT_EQUAL(500, cap.status);
    bb_http_host_capture_free(&cap);

    // Restore a clean global bb_data table: this test's 8 dummy bindings
    // would otherwise starve BB_DATA_MAX_BINDINGS for every test that runs
    // afterward in this same process (bb_data's binding table is a single
    // global static, not reset automatically between tests).
    bb_data_test_reset();
}

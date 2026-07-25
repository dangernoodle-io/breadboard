#include "unity.h"
#include "test_openapi_capture.h"
#include "bb_http_server.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Self-test for test_openapi_capture()/test_openapi_capture_free() (B1-1054
// PR2). Proves the shared helper itself works correctly -- does NOT repoint
// any existing test onto it (that is PR 3-5).
// ---------------------------------------------------------------------------

static bb_err_t stub_handler(bb_http_request_t *req) { (void)req; return BB_OK; }

void test_openapi_capture_returns_parsed_doc_on_success(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t result = test_openapi_capture(&meta);

    TEST_ASSERT_EQUAL(BB_OK, result.status);
    TEST_ASSERT_NOT_NULL(result.cap.body);
    TEST_ASSERT_NOT_NULL(result.doc);

    cJSON *title = cJSON_GetObjectItem(cJSON_GetObjectItem(result.doc, "info"), "title");
    TEST_ASSERT_NOT_NULL(title);
    TEST_ASSERT_EQUAL_STRING("Test", cJSON_GetStringValue(title));

    test_openapi_capture_free(&result);
}

// status != BB_OK -- the emitter itself failed before anything was captured.
void test_openapi_capture_signals_emitter_failure_via_status(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();

    bb_http_host_force_send_chunk_fail(true);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t result = test_openapi_capture(&meta);

    TEST_ASSERT_NOT_EQUAL(BB_OK, result.status);
    TEST_ASSERT_NULL(result.cap.body);
    TEST_ASSERT_NULL(result.doc);

    bb_http_host_force_send_chunk_fail(false);
    test_openapi_capture_free(&result);
}

// status == BB_OK, body non-empty, but the body fails to parse: a malformed
// schema literal is spliced verbatim by the stream path (bb_openapi_emit.c
// splices route->responses[].schema via bb_http_resp_json_obj_set_raw with
// no validation), so the emitter reports success while the resulting
// document is broken JSON. This is the "emit succeeded, output broken"
// regression shape -- distinct from a deliberately forced emitter failure.
static const bb_route_response_t s_malformed_schema_responses[] = {
    { .status = 200, .content_type = "application/json",
      .schema = "{not valid json", .description = "malformed on purpose" },
    { .status = 0 },
};

static const bb_route_t s_route_malformed_schema = {
    .method = BB_HTTP_GET, .path = "/api/broken", .tag = "broken",
    .summary = "Malformed schema literal", .operation_id = "getBroken",
    .responses = s_malformed_schema_responses, .handler = stub_handler,
};

void test_openapi_capture_signals_parse_failure_on_malformed_body(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();
    bb_http_register_described_route(NULL, &s_route_malformed_schema);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t result = test_openapi_capture(&meta);

    TEST_ASSERT_EQUAL(BB_OK, result.status);       // emitter itself succeeded
    TEST_ASSERT_NOT_NULL(result.cap.body);          // body was captured
    TEST_ASSERT_TRUE(result.cap.body_len > 0);
    TEST_ASSERT_NULL(result.doc);                   // but it isn't valid JSON

    test_openapi_capture_free(&result);
    bb_http_route_registry_clear();
}

void test_openapi_capture_free_is_idempotent(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t result = test_openapi_capture(&meta);
    TEST_ASSERT_NOT_NULL(result.doc);

    test_openapi_capture_free(&result);
    TEST_ASSERT_NULL(result.doc);
    TEST_ASSERT_NULL(result.cap.body);

    // Second free on the same (now-zeroed) result must not crash.
    test_openapi_capture_free(&result);
}

void test_openapi_capture_free_handles_null_result(void)
{
    test_openapi_capture_free(NULL);
}

// Guards the single-slot invariant: a second test_openapi_capture() call
// while the first result is still live must fail loudly (TEST_FAIL_MESSAGE)
// instead of silently overwriting s_live and orphaning the first capture.
// TEST_PROTECT()/Unity.CurrentTestFailed catch the expected abort locally so
// this self-test itself still reports a pass. stdout is swapped to
// /dev/null for the guarded call: PlatformIO's own (untrusted, substring-
// scanning) test reader flags any "FAIL:" text it sees regardless of the
// test's real outcome (exactly the misreading run_host_tests.py's direct-
// binary check exists to route around), and that misreading surfaces as a
// nonzero `pio test` exit code -- which run_host_tests.py DOES still trust,
// short-circuiting before the authoritative direct-binary verdict ever runs.
// Suppressing the expected message keeps this true-positive guard test from
// tripping that false negative.
void test_openapi_capture_guards_double_capture_without_free(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t first = test_openapi_capture(&meta);
    TEST_ASSERT_NOT_NULL(first.doc);

    fflush(stdout);
    int saved_stdout = dup(STDOUT_FILENO);
    TEST_ASSERT_TRUE_MESSAGE(saved_stdout >= 0, "dup(STDOUT_FILENO) failed while saving stdout");
    int devnull = open("/dev/null", O_WRONLY);
    TEST_ASSERT_TRUE_MESSAGE(devnull >= 0, "open(\"/dev/null\") failed while redirecting stdout");
    TEST_ASSERT_TRUE_MESSAGE(dup2(devnull, STDOUT_FILENO) >= 0, "dup2() to /dev/null failed while redirecting stdout");

    bool guard_fired = false;
    if (TEST_PROTECT()) {
        test_openapi_capture(&meta); // expected to longjmp via the guard above
    } else {
        guard_fired = true;
        Unity.CurrentTestFailed = 0; // the guard's abort is this test's expected outcome, not a failure of it
    }

    fflush(stdout);
    TEST_ASSERT_TRUE_MESSAGE(dup2(saved_stdout, STDOUT_FILENO) >= 0,
        "dup2() restoring stdout failed -- subsequent test output may be silently lost");
    close(saved_stdout);
    close(devnull);

    TEST_ASSERT_TRUE_MESSAGE(guard_fired,
        "test_openapi_capture() must refuse a second capture while one is still live");

    test_openapi_capture_free(&first);
}

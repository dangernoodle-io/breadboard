#include "test_openapi_capture.h"

#include "unity.h"

// Shadow of the most recently produced, not-yet-explicitly-freed result.
// test_openapi_capture() records into it; test_openapi_capture_free() clears
// it whenever it frees the result it shadows (matched by pointer value, since
// the result struct itself is a caller-owned local -- this file only tracks
// the heap pointers inside it). test_openapi_capture_teardown() uses this
// shadow to close the leak class described in test_openapi_capture.h.
static test_openapi_capture_result_t s_live;

test_openapi_capture_result_t test_openapi_capture(const bb_openapi_meta_t *meta)
{
    // Single-slot invariant: a prior capture must be freed (explicitly, or via
    // tearDown) before a new one begins, or it is silently overwritten here,
    // orphaning its doc/cap.body -- exactly the leak class this helper exists
    // to close. Fail loudly instead of overwriting.
    if (s_live.doc != NULL || s_live.cap.body != NULL) {
        TEST_FAIL_MESSAGE("test_openapi_capture: a previous capture is still live -- "
                           "free it (test_openapi_capture_free) before capturing again");
    }

    test_openapi_capture_result_t result = {0};

    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    result.status = bb_openapi_emit_stream(req, meta);

    // The host capture model is single-slot/synchronous -- capture_end can't
    // practically fail here, but this helper exists to replace ad hoc
    // discarded returns, so it doesn't add one of its own.
    bb_err_t end_status = bb_http_host_capture_end(req, &result.cap);
    TEST_ASSERT_EQUAL(BB_OK, end_status);

    if (result.status == BB_OK && result.cap.body != NULL && result.cap.body_len != 0) {
        result.doc = cJSON_Parse(result.cap.body);
    }

    s_live = result;
    return result;
}

void test_openapi_capture_free(test_openapi_capture_result_t *result)
{
    if (result == NULL) {
        return;
    }

    bool matches_live = (result->doc == s_live.doc) && (result->cap.body == s_live.cap.body);

    if (result->doc != NULL) {
        cJSON_Delete(result->doc);
        result->doc = NULL;
    }

    bb_http_host_capture_free(&result->cap);

    if (matches_live) {
        s_live = (test_openapi_capture_result_t){0};
    }
}

void test_openapi_capture_teardown(void)
{
    if (s_live.doc == NULL && s_live.cap.body == NULL) {
        return; // nothing live -- explicit free already ran, or capture never happened
    }

    test_openapi_capture_result_t snapshot = s_live;
    test_openapi_capture_free(&snapshot); // frees + clears s_live (matches by pointer)
}

#include "test_openapi_capture.h"

#include "unity.h"

test_openapi_capture_result_t test_openapi_capture(const bb_openapi_meta_t *meta)
{
    test_openapi_capture_result_t result = {0};

    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    result.status = bb_openapi_emit_stream(req, meta);

    // The host capture model is single-slot/synchronous -- capture_end can't
    // practically fail here, but this helper exists to replace ad hoc
    // discarded returns, so it doesn't add one of its own.
    bb_err_t end_status = bb_http_host_capture_end(req, &result.cap);
    TEST_ASSERT_EQUAL(BB_OK, end_status);

    if (result.status != BB_OK) {
        return result; // doc stays NULL -- emitter itself failed
    }

    if (result.cap.body == NULL || result.cap.body_len == 0) {
        return result; // doc stays NULL -- emitter ok, nothing captured
    }

    result.doc = cJSON_Parse(result.cap.body);
    return result; // doc NULL here too if cJSON_Parse failed (status stays BB_OK)
}

void test_openapi_capture_free(test_openapi_capture_result_t *result)
{
    if (result == NULL) {
        return;
    }

    if (result->doc != NULL) {
        cJSON_Delete(result->doc);
        result->doc = NULL;
    }

    bb_http_host_capture_free(&result->cap);
}

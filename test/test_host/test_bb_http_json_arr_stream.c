#include "unity.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_json.h"
#include "bb_json_test_hooks.h"
#include <string.h>

// Test: bb_http_resp_json_arr_begin rejects NULL req
void test_json_arr_begin_null_req(void)
{
    bb_http_json_stream_t stream;
    bb_err_t err = bb_http_resp_json_arr_begin(NULL, &stream);
    TEST_ASSERT_EQUAL(err, BB_ERR_INVALID_ARG);
}

// Test: bb_http_resp_json_arr_begin rejects NULL stream
void test_json_arr_begin_null_stream(void)
{
    bb_http_request_t *req = (bb_http_request_t *)&(int){42};  // non-NULL dummy
    bb_err_t err = bb_http_resp_json_arr_begin(req, NULL);
    TEST_ASSERT_EQUAL(err, BB_ERR_INVALID_ARG);
}

// Test: bb_http_resp_json_arr_begin initializes stream correctly
void test_json_arr_begin_init(void)
{
    bb_http_request_t *req = (bb_http_request_t *)&(int){42};
    bb_http_json_stream_t stream;
    bb_err_t err = bb_http_resp_json_arr_begin(req, &stream);
    TEST_ASSERT_EQUAL(err, BB_OK);
    TEST_ASSERT_EQUAL(stream._err, BB_OK);
    TEST_ASSERT_EQUAL(stream._first, 1);
    TEST_ASSERT_EQUAL(stream._open, 1);
    TEST_ASSERT_EQUAL_PTR(stream._req, req);
}

// Test: emit on unopened stream returns BB_ERR_INVALID_STATE
void test_json_arr_emit_unopened(void)
{
    bb_http_json_stream_t stream;
    memset(&stream, 0, sizeof(stream));  // unopened (_open = 0)

    bb_json_t item = bb_json_obj_new();
    bb_json_obj_set_string(item, "k", "v");

    bb_err_t err = bb_http_resp_json_arr_emit(&stream, item);
    TEST_ASSERT_EQUAL(err, BB_ERR_INVALID_STATE);

    bb_json_free(item);
}

// Test: emit after end returns BB_ERR_INVALID_STATE
void test_json_arr_emit_after_end(void)
{
    bb_http_request_t *req = (bb_http_request_t *)&(int){42};
    bb_http_json_stream_t stream;
    bb_http_resp_json_arr_begin(req, &stream);

    // Call end (closes the stream)
    bb_http_resp_json_arr_end(&stream);
    TEST_ASSERT_EQUAL(stream._open, 0);

    // Try to emit after end
    bb_json_t item = bb_json_obj_new();
    bb_json_obj_set_string(item, "k", "v");

    bb_err_t err = bb_http_resp_json_arr_emit(&stream, item);
    TEST_ASSERT_EQUAL(err, BB_ERR_INVALID_STATE);

    bb_json_free(item);
}

// Test: sticky error — first failed emit poisons subsequent emits
void test_json_arr_sticky_error(void)
{
    bb_http_request_t *req = (bb_http_request_t *)&(int){42};
    bb_http_json_stream_t stream;
    bb_http_resp_json_arr_begin(req, &stream);

    // Manually set an error to simulate a failed emit
    stream._err = BB_ERR_NO_SPACE;

    // Second emit should return the sticky error (no-op)
    bb_json_t item = bb_json_obj_new();
    bb_json_obj_set_string(item, "k", "v");

    bb_err_t err = bb_http_resp_json_arr_emit(&stream, item);
    TEST_ASSERT_EQUAL(err, BB_ERR_NO_SPACE);

    bb_json_free(item);

    // End should return the sticky error
    err = bb_http_resp_json_arr_end(&stream);
    TEST_ASSERT_EQUAL(err, BB_ERR_NO_SPACE);
}

// Test: empty array (begin → end with no emit) produces valid structure
void test_json_arr_empty(void)
{
    bb_http_request_t *req = (bb_http_request_t *)&(int){42};
    bb_http_json_stream_t stream;

    bb_err_t err = bb_http_resp_json_arr_begin(req, &stream);
    TEST_ASSERT_EQUAL(err, BB_OK);

    err = bb_http_resp_json_arr_end(&stream);
    TEST_ASSERT_EQUAL(err, BB_OK);
    TEST_ASSERT_EQUAL(stream._open, 0);
}

// Test: single-item stream
void test_json_arr_single_item(void)
{
    bb_http_request_t *req = (bb_http_request_t *)&(int){42};
    bb_http_json_stream_t stream;

    bb_err_t err = bb_http_resp_json_arr_begin(req, &stream);
    TEST_ASSERT_EQUAL(err, BB_OK);

    bb_json_t item = bb_json_obj_new();
    bb_json_obj_set_string(item, "name", "alice");
    bb_json_obj_set_number(item, "age", 30);

    err = bb_http_resp_json_arr_emit(&stream, item);
    TEST_ASSERT_EQUAL(err, BB_OK);
    TEST_ASSERT_EQUAL(stream._first, 0);  // _first now 0 after first emit

    bb_json_free(item);

    err = bb_http_resp_json_arr_end(&stream);
    TEST_ASSERT_EQUAL(err, BB_OK);
}

// Test: multi-item stream
void test_json_arr_multiple_items(void)
{
    bb_http_request_t *req = (bb_http_request_t *)&(int){42};
    bb_http_json_stream_t stream;

    bb_err_t err = bb_http_resp_json_arr_begin(req, &stream);
    TEST_ASSERT_EQUAL(err, BB_OK);

    // Emit three items
    for (int i = 0; i < 3; i++) {
        bb_json_t item = bb_json_obj_new();
        bb_json_obj_set_number(item, "id", i);
        bb_json_obj_set_string(item, "name", i == 0 ? "alice" : i == 1 ? "bob" : "charlie");

        err = bb_http_resp_json_arr_emit(&stream, item);
        TEST_ASSERT_EQUAL(err, BB_OK);

        bb_json_free(item);
    }

    err = bb_http_resp_json_arr_end(&stream);
    TEST_ASSERT_EQUAL(err, BB_OK);
}

// Test: emit with NULL item (graceful handling)
void test_json_arr_emit_null_item(void)
{
    bb_http_request_t *req = (bb_http_request_t *)&(int){42};
    bb_http_json_stream_t stream;

    bb_err_t err = bb_http_resp_json_arr_begin(req, &stream);
    TEST_ASSERT_EQUAL(err, BB_OK);

    // Emit NULL item — should handle gracefully (serialize returns NULL → "null")
    err = bb_http_resp_json_arr_emit(&stream, NULL);
    // On host/Arduino backends, this may succeed (appending "null" string)
    // On ESP-IDF, serialize will fail but emit literal "null"
    // Either way, the stream should remain valid

    err = bb_http_resp_json_arr_end(&stream);
    TEST_ASSERT_EQUAL(err, BB_OK);
}

// Test: emit NULL stream returns BB_ERR_INVALID_ARG
void test_json_arr_emit_null_stream(void)
{
    bb_json_t item = bb_json_obj_new();
    bb_err_t err = bb_http_resp_json_arr_emit(NULL, item);
    TEST_ASSERT_EQUAL(err, BB_ERR_INVALID_ARG);
    bb_json_free(item);
}

// Test: end with NULL stream returns BB_ERR_INVALID_ARG
void test_json_arr_end_null_stream(void)
{
    bb_err_t err = bb_http_resp_json_arr_end(NULL);
    TEST_ASSERT_EQUAL(err, BB_ERR_INVALID_ARG);
}

// Test: emit against a stream whose _req was never registered via begin()
// (stream_get_arr's side-table lookup misses) returns BB_ERR_INVALID_STATE.
// Exercises the "no matching entry" fallthrough of the internal side-table
// helper, distinct from test_json_arr_emit_after_end (which short-circuits
// earlier on the _open==0 check).
void test_json_arr_emit_unregistered_req_returns_invalid_state(void)
{
    bb_http_json_stream_t stream;
    memset(&stream, 0, sizeof(stream));
    bb_http_request_t *unregistered_req = (bb_http_request_t *)&(int){99};
    stream._req  = unregistered_req;
    stream._err  = BB_OK;
    stream._open = 1;

    bb_json_t item = bb_json_obj_new();
    bb_err_t err = bb_http_resp_json_arr_emit(&stream, item);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);

    bb_json_free(item);
}

// Test: end() falls back to "null" element when bb_json_serialize fails on
// the buffered array — exercises bb_http_resp_send_json's serialize-failure
// fallback (internal to the host backend, only reachable via end()).
void test_json_arr_end_serialize_failure_falls_back_to_null(void)
{
    bb_http_request_t *req = (bb_http_request_t *)&(int){42};
    bb_http_json_stream_t stream;

    bb_err_t err = bb_http_resp_json_arr_begin(req, &stream);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_json_host_force_serialize_fail_after(0);
    err = bb_http_resp_json_arr_end(&stream);
    bb_json_host_force_serialize_fail_after(-1);

    TEST_ASSERT_EQUAL(BB_OK, err);
}

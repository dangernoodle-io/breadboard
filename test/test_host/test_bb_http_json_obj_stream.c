#include "unity.h"
#include "bb_http.h"
#include "bb_http_host.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Capture a response from a handler-like block; caller provides the req pointer.
static bb_http_host_capture_t s_cap;
static bb_http_request_t *s_req;

static void cap_begin(void)
{
    bb_http_host_capture_begin(&s_req);
}

static void cap_end(void)
{
    bb_http_host_capture_end(s_req, &s_cap);
}

static void cap_free(void)
{
    bb_http_host_capture_free(&s_cap);
}

// ---------------------------------------------------------------------------
// begin / argument validation
// ---------------------------------------------------------------------------

void test_json_obj_begin_null_req(void)
{
    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(NULL, &obj);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_json_obj_begin_null_stream(void)
{
    bb_http_request_t *req = (bb_http_request_t *)&(int){42};
    bb_err_t err = bb_http_resp_json_obj_begin(req, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_json_obj_begin_init(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(s_req, &obj);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(BB_OK, obj._err);
    TEST_ASSERT_EQUAL(1,     obj._open);
    TEST_ASSERT_EQUAL(0,     obj._depth);
    TEST_ASSERT_EQUAL_PTR(s_req, obj._req);
    // Clean up
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

// ---------------------------------------------------------------------------
// empty object
// ---------------------------------------------------------------------------

void test_json_obj_empty(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_err_t err = bb_http_resp_json_obj_end(&obj);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(0, obj._open);
    cap_end();
    TEST_ASSERT_EQUAL_STRING("{}", s_cap.body);
    cap_free();
}

// ---------------------------------------------------------------------------
// set_str
// ---------------------------------------------------------------------------

void test_json_obj_set_str_basic(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "name", "alice");
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    TEST_ASSERT_EQUAL_STRING("{\"name\":\"alice\"}", s_cap.body);
    cap_free();
}

void test_json_obj_set_str_null_val(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "x", NULL);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    TEST_ASSERT_EQUAL_STRING("{\"x\":null}", s_cap.body);
    cap_free();
}

void test_json_obj_set_str_escape(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "msg", "say \"hi\"\nnewline\ttab\\back");
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    TEST_ASSERT_EQUAL_STRING(
        "{\"msg\":\"say \\\"hi\\\"\\nnewline\\ttab\\\\back\"}",
        s_cap.body);
    cap_free();
}

// ---------------------------------------------------------------------------
// set_int
// ---------------------------------------------------------------------------

void test_json_obj_set_int(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_int(&obj, "count", 42);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    TEST_ASSERT_EQUAL_STRING("{\"count\":42}", s_cap.body);
    cap_free();
}

void test_json_obj_set_int_negative(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_int(&obj, "delta", -7);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    TEST_ASSERT_EQUAL_STRING("{\"delta\":-7}", s_cap.body);
    cap_free();
}

// ---------------------------------------------------------------------------
// set_bool
// ---------------------------------------------------------------------------

void test_json_obj_set_bool_true(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_bool(&obj, "ok", true);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true}", s_cap.body);
    cap_free();
}

void test_json_obj_set_bool_false(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_bool(&obj, "ok", false);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    TEST_ASSERT_EQUAL_STRING("{\"ok\":false}", s_cap.body);
    cap_free();
}

// ---------------------------------------------------------------------------
// set_null
// ---------------------------------------------------------------------------

void test_json_obj_set_null(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_null(&obj, "ptr");
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    TEST_ASSERT_EQUAL_STRING("{\"ptr\":null}", s_cap.body);
    cap_free();
}

// ---------------------------------------------------------------------------
// multiple fields — comma handling
// ---------------------------------------------------------------------------

void test_json_obj_multiple_fields(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_str( &obj, "a", "x");
    bb_http_resp_json_obj_set_int( &obj, "b", 1);
    bb_http_resp_json_obj_set_bool(&obj, "c", true);
    bb_http_resp_json_obj_set_null(&obj, "d");
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    TEST_ASSERT_EQUAL_STRING(
        "{\"a\":\"x\",\"b\":1,\"c\":true,\"d\":null}",
        s_cap.body);
    cap_free();
}

// ---------------------------------------------------------------------------
// nested object
// ---------------------------------------------------------------------------

void test_json_obj_nested_obj(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "top", "val");
    bb_http_resp_json_obj_set_obj_begin(&obj, "inner");
    bb_http_resp_json_obj_set_int(&obj, "x", 1);
    bb_http_resp_json_obj_set_obj_end(&obj);
    bb_http_resp_json_obj_set_str(&obj, "after", "y");
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    TEST_ASSERT_EQUAL_STRING(
        "{\"top\":\"val\",\"inner\":{\"x\":1},\"after\":\"y\"}",
        s_cap.body);
    cap_free();
}

// ---------------------------------------------------------------------------
// nested array
// ---------------------------------------------------------------------------

void test_json_obj_nested_arr(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_arr_begin(&obj, "items");
    // Inside the array, emit elements using set_str/set_int with the
    // depth-comma tracking; key is ignored when emitting raw array elements.
    // We call set_* with NULL key for array elements.
    bb_http_resp_json_obj_set_str(&obj, NULL, "a");
    bb_http_resp_json_obj_set_str(&obj, NULL, "b");
    bb_http_resp_json_obj_set_arr_end(&obj);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    // Array elements: no "key": prefix since we passed NULL key
    TEST_ASSERT_EQUAL_STRING(
        "{\"items\":[\"a\",\"b\"]}",
        s_cap.body);
    cap_free();
}

// ---------------------------------------------------------------------------
// guard: operations on closed stream
// ---------------------------------------------------------------------------

void test_json_obj_set_after_end(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_end(&obj);
    TEST_ASSERT_EQUAL(0, obj._open);

    bb_err_t err = bb_http_resp_json_obj_set_str(&obj, "x", "y");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    cap_end();
    cap_free();
}

// ---------------------------------------------------------------------------
// sticky error propagation
// ---------------------------------------------------------------------------

void test_json_obj_sticky_error(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);

    // Inject a sticky error
    obj._err = BB_ERR_NO_SPACE;

    bb_err_t err = bb_http_resp_json_obj_set_str(&obj, "k", "v");
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);

    err = bb_http_resp_json_obj_end(&obj);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    cap_end();
    cap_free();
}

// ---------------------------------------------------------------------------
// NULL stream guards
// ---------------------------------------------------------------------------

void test_json_obj_set_str_null_stream(void)
{
    bb_err_t err = bb_http_resp_json_obj_set_str(NULL, "k", "v");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_json_obj_set_int_null_stream(void)
{
    bb_err_t err = bb_http_resp_json_obj_set_int(NULL, "k", 0);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_json_obj_set_bool_null_stream(void)
{
    bb_err_t err = bb_http_resp_json_obj_set_bool(NULL, "k", false);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_json_obj_set_null_null_stream(void)
{
    bb_err_t err = bb_http_resp_json_obj_set_null(NULL, "k");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_json_obj_end_null_stream(void)
{
    bb_err_t err = bb_http_resp_json_obj_end(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

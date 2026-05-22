#include "unity.h"
#include "bb_http.h"
#include "bb_http_host.h"
#include <string.h>
#include <stdio.h>

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

// ---------------------------------------------------------------------------
// set_num
// ---------------------------------------------------------------------------

void test_json_obj_set_num_basic(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_num(&obj, "pi", 3.14);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    TEST_ASSERT_EQUAL_STRING("{\"pi\":3.14}", s_cap.body);
    cap_free();
}

void test_json_obj_set_num_integer_value(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_num(&obj, "n", 42.0);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    TEST_ASSERT_EQUAL_STRING("{\"n\":42}", s_cap.body);
    cap_free();
}

void test_json_obj_set_num_null_stream(void)
{
    bb_err_t err = bb_http_resp_json_obj_set_num(NULL, "k", 1.0);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_json_obj_set_num_not_open(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_end(&obj);

    bb_err_t err = bb_http_resp_json_obj_set_num(&obj, "k", 1.0);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    cap_end();
    cap_free();
}

void test_json_obj_set_num_sticky_error(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    obj._err = BB_ERR_NO_SPACE;

    bb_err_t err = bb_http_resp_json_obj_set_num(&obj, "k", 1.0);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

// ---------------------------------------------------------------------------
// string escape — \r and control characters
// ---------------------------------------------------------------------------

void test_json_obj_set_str_escape_cr(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "msg", "line1\rline2");
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    TEST_ASSERT_EQUAL_STRING("{\"msg\":\"line1\\rline2\"}", s_cap.body);
    cap_free();
}

void test_json_obj_set_str_escape_ctrl(void)
{
    // ASCII BEL (0x07) is a control char < 0x20, not \n/\r/\t
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "x", "\x07");
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    TEST_ASSERT_EQUAL_STRING("{\"x\":\"\\u0007\"}", s_cap.body);
    cap_free();
}

// ---------------------------------------------------------------------------
// depth overflow — set_obj_begin and set_arr_begin at max depth
// ---------------------------------------------------------------------------

void test_json_obj_set_obj_begin_depth_overflow(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);

    // Nest 7 levels deep (depth goes 0→7); the 8th begin must fail
    for (int i = 0; i < 7; i++) {
        bb_err_t e = bb_http_resp_json_obj_set_obj_begin(&obj, "a");
        TEST_ASSERT_EQUAL_MESSAGE(BB_OK, e, "unexpected error before max depth");
    }
    bb_err_t err = bb_http_resp_json_obj_set_obj_begin(&obj, "overflow");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    // Stream is now poisoned; end returns the error
    err = bb_http_resp_json_obj_end(&obj);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    cap_end();
    cap_free();
}

void test_json_obj_set_arr_begin_depth_overflow(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);

    for (int i = 0; i < 7; i++) {
        bb_err_t e = bb_http_resp_json_obj_set_arr_begin(&obj, "a");
        TEST_ASSERT_EQUAL_MESSAGE(BB_OK, e, "unexpected error before max depth");
    }
    bb_err_t err = bb_http_resp_json_obj_set_arr_begin(&obj, "overflow");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    err = bb_http_resp_json_obj_end(&obj);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    cap_end();
    cap_free();
}

// ---------------------------------------------------------------------------
// depth underflow — end when depth is already 0
// ---------------------------------------------------------------------------

void test_json_obj_set_obj_end_underflow(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);

    // No matching begin — depth is already 0
    bb_err_t err = bb_http_resp_json_obj_set_obj_end(&obj);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_set_arr_end_underflow(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);

    bb_err_t err = bb_http_resp_json_obj_set_arr_end(&obj);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

// ---------------------------------------------------------------------------
// obj_maybe_comma depth-guard (depth manually set to MAX)
// ---------------------------------------------------------------------------

void test_json_obj_maybe_comma_depth_guard(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);

    // Force depth to 8 (= BB_JSON_OBJ_MAX_DEPTH) to trigger the guard
    obj._depth = 8;
    bb_err_t err = bb_http_resp_json_obj_set_str(&obj, "k", "v");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, obj._err);
    // Reset depth so end() can flush without crashing
    obj._depth = 0;
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

// ---------------------------------------------------------------------------
// buffer flush — trigger mid-stream flush by filling the buffer
// ---------------------------------------------------------------------------

void test_json_obj_buffer_flush_on_overflow(void)
{
    // Write enough data to overflow the 1024-byte internal buffer.
    // Each field emits: ,"key":VALUE. We write many small string fields.
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);

    // 100 fields of ~12 bytes each = ~1200 bytes, exceeding the 1024 buffer
    for (int i = 0; i < 100; i++) {
        char key[8];
        snprintf(key, sizeof(key), "k%d", i);
        bb_err_t e = bb_http_resp_json_obj_set_str(&obj, key, "val");
        TEST_ASSERT_EQUAL(BB_OK, e);
    }
    bb_err_t err = bb_http_resp_json_obj_end(&obj);
    TEST_ASSERT_EQUAL(BB_OK, err);
    cap_end();
    // Verify the body starts/ends correctly
    TEST_ASSERT_NOT_NULL(s_cap.body);
    TEST_ASSERT_EQUAL('{', s_cap.body[0]);
    TEST_ASSERT_EQUAL('}', s_cap.body[s_cap.body_len - 1]);
    cap_free();
}

// ---------------------------------------------------------------------------
// large token — string > BB_HTTP_JSON_OBJ_BUF_SIZE triggers direct send
// ---------------------------------------------------------------------------

void test_json_obj_large_string_direct_send(void)
{
    // A string value > 1024 bytes bypasses the buffer (direct send path)
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);

    char big[1100];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    bb_err_t err = bb_http_resp_json_obj_set_str(&obj, "data", big);
    TEST_ASSERT_EQUAL(BB_OK, err);
    err = bb_http_resp_json_obj_end(&obj);
    TEST_ASSERT_EQUAL(BB_OK, err);
    cap_end();
    TEST_ASSERT_NOT_NULL(s_cap.body);
    // Body must contain all 1099 'A' chars
    TEST_ASSERT_NOT_NULL(strstr(s_cap.body, big));
    cap_free();
}

// ---------------------------------------------------------------------------
// begin fails when set_type fails (line 141)
// ---------------------------------------------------------------------------

void test_json_obj_begin_set_type_fail(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_host_force_set_type_fail(true);
    bb_err_t err = bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_host_force_set_type_fail(false);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    cap_end();
    cap_free();
}

// ---------------------------------------------------------------------------
// begin fails when initial send_chunk("{") fails (lines 149-150)
// ---------------------------------------------------------------------------

void test_json_obj_begin_send_chunk_fail(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_host_force_send_chunk_fail(true);
    bb_err_t err = bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_host_force_send_chunk_fail(false);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, obj._err);
    cap_end();
    cap_free();
}

// ---------------------------------------------------------------------------
// end() terminal send_chunk(NULL,0) failure propagates (lines 319-320)
// ---------------------------------------------------------------------------

void test_json_obj_end_term_chunk_fail(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "k", "v");

    // Force only the terminator call (buf==NULL) to fail; flush proceeds OK.
    bb_http_host_force_send_chunk_term_fail(true);
    bb_err_t err = bb_http_resp_json_obj_end(&obj);
    bb_http_host_force_send_chunk_term_fail(false);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    cap_end();
    cap_free();
}

// ---------------------------------------------------------------------------
// send_chunk failure propagates through flush path
// ---------------------------------------------------------------------------

void test_json_obj_flush_send_chunk_fail(void)
{
    // Inject a send_chunk failure mid-stream by filling the buffer to near
    // capacity and then enabling the failure hook before the overflow flush.
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);

    // Write fields until we have >= 900 bytes in the buffer (near the 1024
    // limit). 90 fields of ~11 bytes each = ~990 bytes.
    for (int i = 0; i < 90; i++) {
        char key[8];
        snprintf(key, sizeof(key), "k%d", i);
        bb_http_resp_json_obj_set_str(&obj, key, "v");
    }

    // Inject failure. The next write that pushes buf_len + len > 1024 will
    // call obj_flush which calls send_chunk and receives the error.
    bb_http_host_force_send_chunk_fail(true);
    bb_err_t err = bb_http_resp_json_obj_set_str(&obj, "trigger", "overflow");
    bb_http_host_force_send_chunk_fail(false);

    // If the flush was triggered, err != BB_OK and _err is poisoned.
    // If the buffer wasn't full enough, force the flush path via end().
    if (err == BB_OK) {
        // buf_len is still > 0; end() calls obj_flush with fail hook off —
        // it will succeed. Instead, re-enable the hook for the flush in end().
        bb_http_host_force_send_chunk_fail(true);
        err = bb_http_resp_json_obj_end(&obj);
        bb_http_host_force_send_chunk_fail(false);
    } else {
        bb_http_resp_json_obj_end(&obj);
    }
    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    cap_end();
    cap_free();
}

// ---------------------------------------------------------------------------
// NULL / closed / sticky-error guards for set_int, set_bool, set_null
// ---------------------------------------------------------------------------

void test_json_obj_set_int_not_open(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_end(&obj);
    bb_err_t err = bb_http_resp_json_obj_set_int(&obj, "k", 1);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    cap_end();
    cap_free();
}

void test_json_obj_set_int_sticky_error(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    obj._err = BB_ERR_NO_SPACE;
    bb_err_t err = bb_http_resp_json_obj_set_int(&obj, "k", 1);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_set_bool_not_open(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_end(&obj);
    bb_err_t err = bb_http_resp_json_obj_set_bool(&obj, "k", true);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    cap_end();
    cap_free();
}

void test_json_obj_set_bool_sticky_error(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    obj._err = BB_ERR_NO_SPACE;
    bb_err_t err = bb_http_resp_json_obj_set_bool(&obj, "k", false);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_set_null_not_open(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_end(&obj);
    bb_err_t err = bb_http_resp_json_obj_set_null(&obj, "k");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    cap_end();
    cap_free();
}

void test_json_obj_set_null_sticky_error(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    obj._err = BB_ERR_NO_SPACE;
    bb_err_t err = bb_http_resp_json_obj_set_null(&obj, "k");
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

// ---------------------------------------------------------------------------
// NULL / closed / sticky-error guards for set_obj_begin/end, set_arr_begin/end
// ---------------------------------------------------------------------------

void test_json_obj_set_obj_begin_null_stream(void)
{
    bb_err_t err = bb_http_resp_json_obj_set_obj_begin(NULL, "k");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_json_obj_set_obj_begin_not_open(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_end(&obj);
    bb_err_t err = bb_http_resp_json_obj_set_obj_begin(&obj, "k");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    cap_end();
    cap_free();
}

void test_json_obj_set_obj_begin_sticky_error(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    obj._err = BB_ERR_NO_SPACE;
    bb_err_t err = bb_http_resp_json_obj_set_obj_begin(&obj, "k");
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_set_obj_end_null_stream(void)
{
    bb_err_t err = bb_http_resp_json_obj_set_obj_end(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_json_obj_set_obj_end_not_open(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_end(&obj);
    bb_err_t err = bb_http_resp_json_obj_set_obj_end(&obj);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    cap_end();
    cap_free();
}

void test_json_obj_set_obj_end_sticky_error(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_obj_begin(&obj, "inner");
    obj._err = BB_ERR_NO_SPACE;
    bb_err_t err = bb_http_resp_json_obj_set_obj_end(&obj);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_set_arr_begin_null_stream(void)
{
    bb_err_t err = bb_http_resp_json_obj_set_arr_begin(NULL, "k");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_json_obj_set_arr_begin_not_open(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_end(&obj);
    bb_err_t err = bb_http_resp_json_obj_set_arr_begin(&obj, "k");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    cap_end();
    cap_free();
}

void test_json_obj_set_arr_begin_sticky_error(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    obj._err = BB_ERR_NO_SPACE;
    bb_err_t err = bb_http_resp_json_obj_set_arr_begin(&obj, "k");
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_set_arr_end_null_stream(void)
{
    bb_err_t err = bb_http_resp_json_obj_set_arr_end(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_json_obj_set_arr_end_not_open(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_end(&obj);
    bb_err_t err = bb_http_resp_json_obj_set_arr_end(&obj);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
    cap_end();
    cap_free();
}

void test_json_obj_set_arr_end_sticky_error(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_arr_begin(&obj, "items");
    obj._err = BB_ERR_NO_SPACE;
    bb_err_t err = bb_http_resp_json_obj_set_arr_end(&obj);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

// ---------------------------------------------------------------------------
// Error propagation via buffer-near-full + send_chunk failure.
//
// Pattern: pre-fill the internal buffer to 1023 bytes so the next append
// (len >= 2) overflows the 1024-byte buffer, triggering obj_flush, which
// fails because send_chunk is broken.  This drives the error-return branches
// deep inside obj_append, obj_maybe_comma, obj_emit_str_escaped, obj_emit_key,
// and every public set_* wrapper.
// ---------------------------------------------------------------------------

// Helper: write exactly `n` bytes into stream->_buf without going through the
// public API (so no flush is triggered and no error is recorded).
static void fill_buf(bb_http_json_obj_stream_t *s, size_t n)
{
    memset(s->_buf, 'X', n);
    s->_buf_len = n;
}

void test_json_obj_append_flush_error(void)
{
    // Fill buffer so next 2-byte append triggers overflow flush.
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    fill_buf(&obj, BB_HTTP_JSON_OBJ_BUF_SIZE - 1);  // 1023 bytes in buf

    bb_http_host_force_send_chunk_fail(true);
    // set_null emits "null" (4 bytes) — first append triggers flush+fail
    bb_err_t err = bb_http_resp_json_obj_set_null(&obj, NULL);
    bb_http_host_force_send_chunk_fail(false);

    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    obj._err = BB_OK;
    obj._buf_len = 0;
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_maybe_comma_putc_error(void)
{
    // Get a comma pending, then overflow+fail when the comma is written.
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_null(&obj, "first");  // _needs_comma[0] = 1
    fill_buf(&obj, BB_HTTP_JSON_OBJ_BUF_SIZE);      // pack buf to 1024 (full)

    bb_http_host_force_send_chunk_fail(true);
    // obj_maybe_comma tries to putc(',') — buffer full, triggers flush+fail
    bb_err_t err = bb_http_resp_json_obj_set_null(&obj, "second");
    bb_http_host_force_send_chunk_fail(false);

    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    obj._err = BB_OK;
    obj._buf_len = 0;
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_emit_str_first_quote_error(void)
{
    // Trigger the error path on the opening '"' in obj_emit_str_escaped.
    // Sequence: write one field (sets _needs_comma[0]=1), fill buf to 1023,
    // then write a second field.  obj_maybe_comma writes ',' (buf -> 1024),
    // then obj_emit_str_escaped tries putc('"') which overflows -> flush+fail.
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    bb_http_resp_json_obj_set_null(&obj, "a");  // _needs_comma[0] = 1
    fill_buf(&obj, BB_HTTP_JSON_OBJ_BUF_SIZE - 1);  // 1023 bytes

    bb_http_host_force_send_chunk_fail(true);
    // comma writes to slot 1023 (no overflow), then '"' at slot 1024 overflows
    bb_err_t err = bb_http_resp_json_obj_set_int(&obj, "k", 0);
    bb_http_host_force_send_chunk_fail(false);

    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    obj._err = BB_OK;
    obj._buf_len = 0;
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_emit_str_loop_error(void)
{
    // The key fits in the buffer (first quote + k = 2 bytes from 1022 free),
    // but the loop character write overflows.  Use a 2-char key so that after
    // the opening quote the loop triggers the overflow.
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    fill_buf(&obj, BB_HTTP_JSON_OBJ_BUF_SIZE - 2);  // 1022 bytes; 2 slots free

    bb_http_host_force_send_chunk_fail(true);
    // putc('"') fits (slot 1023), first char of key "ab" overflows (slot 1025)
    bb_err_t err = bb_http_resp_json_obj_set_bool(&obj, "ab", true);
    bb_http_host_force_send_chunk_fail(false);

    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    obj._err = BB_OK;
    obj._buf_len = 0;
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_emit_key_after_str_error(void)
{
    // The key's closing quote fits, but the ':' after obj_emit_str_escaped
    // overflows + fails — driving the err check at the end of obj_emit_key.
    // Use a key that exactly fills the remaining buffer up to the ':'.
    // Buffer = BUF_SIZE - 3; opening '"' + 1 key char + closing '"' = 3 bytes
    // exactly fills the buffer; then ':' overflows.
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    fill_buf(&obj, BB_HTTP_JSON_OBJ_BUF_SIZE - 3);  // 1021 bytes; 3 slots

    bb_http_host_force_send_chunk_fail(true);
    // '"' + 'k' + '"' fills the buffer; ':' triggers flush+fail
    bb_err_t err = bb_http_resp_json_obj_set_num(&obj, "k", 1.0);
    bb_http_host_force_send_chunk_fail(false);

    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    obj._err = BB_OK;
    obj._buf_len = 0;
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_set_obj_begin_emit_key_error(void)
{
    // Emit key for set_obj_begin fails (flush+fail on key emission).
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    fill_buf(&obj, BB_HTTP_JSON_OBJ_BUF_SIZE - 1);

    bb_http_host_force_send_chunk_fail(true);
    bb_err_t err = bb_http_resp_json_obj_set_obj_begin(&obj, "x");
    bb_http_host_force_send_chunk_fail(false);

    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    obj._err = BB_OK;
    obj._buf_len = 0;
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_set_obj_begin_putc_brace_error(void)
{
    // Key fits but the '{' after it triggers overflow+fail.
    // Key "k": '"' + 'k' + '"' + ':' = 4 bytes; use BUF_SIZE-4 to leave 4 free.
    // Then '{' would need slot BUF_SIZE+1 — but actually after the 4-byte key
    // the buffer is full, and '{' triggers a flush.
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    fill_buf(&obj, BB_HTTP_JSON_OBJ_BUF_SIZE - 4);  // 1020 bytes; 4 slots

    bb_http_host_force_send_chunk_fail(true);
    // '"k":' fills slots 1021-1024; '{' needs slot 1025 → flush+fail
    bb_err_t err = bb_http_resp_json_obj_set_obj_begin(&obj, "k");
    bb_http_host_force_send_chunk_fail(false);

    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    obj._err = BB_OK;
    obj._buf_len = 0;
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_set_arr_begin_emit_key_error(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    fill_buf(&obj, BB_HTTP_JSON_OBJ_BUF_SIZE - 1);

    bb_http_host_force_send_chunk_fail(true);
    bb_err_t err = bb_http_resp_json_obj_set_arr_begin(&obj, "x");
    bb_http_host_force_send_chunk_fail(false);

    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    obj._err = BB_OK;
    obj._buf_len = 0;
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_set_arr_begin_putc_bracket_error(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    fill_buf(&obj, BB_HTTP_JSON_OBJ_BUF_SIZE - 4);

    bb_http_host_force_send_chunk_fail(true);
    bb_err_t err = bb_http_resp_json_obj_set_arr_begin(&obj, "k");
    bb_http_host_force_send_chunk_fail(false);

    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    obj._err = BB_OK;
    obj._buf_len = 0;
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_set_str_emit_key_error(void)
{
    // err != BB_OK branch at line "if (err != BB_OK) return err" after obj_emit_key
    // in set_str.
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    fill_buf(&obj, BB_HTTP_JSON_OBJ_BUF_SIZE - 1);

    bb_http_host_force_send_chunk_fail(true);
    bb_err_t err = bb_http_resp_json_obj_set_str(&obj, "x", "v");
    bb_http_host_force_send_chunk_fail(false);

    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    obj._err = BB_OK;
    obj._buf_len = 0;
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_set_int_emit_key_error(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    fill_buf(&obj, BB_HTTP_JSON_OBJ_BUF_SIZE - 1);

    bb_http_host_force_send_chunk_fail(true);
    bb_err_t err = bb_http_resp_json_obj_set_int(&obj, "x", 1);
    bb_http_host_force_send_chunk_fail(false);

    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    obj._err = BB_OK;
    obj._buf_len = 0;
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_set_bool_emit_key_error(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    fill_buf(&obj, BB_HTTP_JSON_OBJ_BUF_SIZE - 1);

    bb_http_host_force_send_chunk_fail(true);
    bb_err_t err = bb_http_resp_json_obj_set_bool(&obj, "x", true);
    bb_http_host_force_send_chunk_fail(false);

    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    obj._err = BB_OK;
    obj._buf_len = 0;
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_set_null_emit_key_error(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    fill_buf(&obj, BB_HTTP_JSON_OBJ_BUF_SIZE - 1);

    bb_http_host_force_send_chunk_fail(true);
    bb_err_t err = bb_http_resp_json_obj_set_null(&obj, "x");
    bb_http_host_force_send_chunk_fail(false);

    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    obj._err = BB_OK;
    obj._buf_len = 0;
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}

void test_json_obj_set_num_emit_key_error(void)
{
    cap_begin();
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(s_req, &obj);
    fill_buf(&obj, BB_HTTP_JSON_OBJ_BUF_SIZE - 1);

    bb_http_host_force_send_chunk_fail(true);
    bb_err_t err = bb_http_resp_json_obj_set_num(&obj, "x", 1.0);
    bb_http_host_force_send_chunk_fail(false);

    TEST_ASSERT_NOT_EQUAL(BB_OK, err);
    obj._err = BB_OK;
    obj._buf_len = 0;
    bb_http_resp_json_obj_end(&obj);
    cap_end();
    cap_free();
}


#include "unity.h"
#include "bb_http.h"
#include "bb_json.h"

static int s_walk_count = 0;

static void count_walker(const char *key, bb_json_t child, void *ctx)
{
    (void)key;
    (void)child;
    (void)ctx;
    s_walk_count++;
}

void test_bb_json_get_kind_object(void)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_kind_t kind = bb_json_get_kind(obj);
    TEST_ASSERT_EQUAL(kind, BB_JSON_KIND_OBJECT);
    bb_json_free(obj);
}

void test_bb_json_get_kind_array(void)
{
    bb_json_t arr = bb_json_arr_new();
    bb_json_kind_t kind = bb_json_get_kind(arr);
    TEST_ASSERT_EQUAL(kind, BB_JSON_KIND_ARRAY);
    bb_json_free(arr);
}

void test_bb_json_get_kind_scalar(void)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_string(obj, "key", "value");
    bb_json_t item = bb_json_obj_get_item(obj, "key");
    bb_json_kind_t kind = bb_json_get_kind(item);
    TEST_ASSERT_EQUAL(kind, BB_JSON_KIND_OTHER);
    bb_json_free(obj);
}

void test_bb_json_walk_children_object(void)
{
    s_walk_count = 0;
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_string(obj, "a", "1");
    bb_json_obj_set_string(obj, "b", "2");
    bb_json_walk_children(obj, count_walker, NULL);
    TEST_ASSERT_EQUAL(s_walk_count, 2);
    bb_json_free(obj);
}

void test_bb_json_walk_children_array(void)
{
    s_walk_count = 0;
    bb_json_t arr = bb_json_arr_new();
    bb_json_arr_append_string(arr, "x");
    bb_json_arr_append_string(arr, "y");
    bb_json_arr_append_string(arr, "z");
    bb_json_walk_children(arr, count_walker, NULL);
    TEST_ASSERT_EQUAL(s_walk_count, 3);
    bb_json_free(arr);
}

void test_bb_http_resp_send_json_callable(void)
{
    // Host stub is a no-op; just verify it's callable and returns OK
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_string(obj, "key", "value");
    bb_err_t err = bb_http_resp_send_json(NULL, obj);
    TEST_ASSERT_EQUAL(err, BB_OK);
    bb_json_free(obj);
}

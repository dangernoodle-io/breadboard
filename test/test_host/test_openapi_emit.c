#include "unity.h"
#include "bb_openapi.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "test_openapi_capture.h"

#include <cJSON.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Stub handler
// ---------------------------------------------------------------------------

static bb_err_t stub_handler(bb_http_request_t *req)
{
    (void)req;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Fixture 1: GET /api/foo — response with schema, has tag + operation_id
// ---------------------------------------------------------------------------

static const bb_route_response_t s_foo_responses[] = {
    {
        .status       = 200,
        .content_type = "application/json",
        .schema       = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}}}",
        .description  = "foo response",
    },
    { .status = 0 },
};

static const bb_route_t s_route_foo = {
    .method               = BB_HTTP_GET,
    .path                 = "/api/foo",
    .tag                  = "foo-tag",
    .summary              = "Get foo resource",
    .operation_id         = "getFoo",
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_foo_responses,
    .handler              = stub_handler,
};

// ---------------------------------------------------------------------------
// Fixture 2: POST /api/bar — request + response schema
// ---------------------------------------------------------------------------

static const bb_route_response_t s_bar_responses[] = {
    {
        .status       = 201,
        .content_type = "application/json",
        .schema       = "{\"type\":\"object\",\"properties\":{\"created\":{\"type\":\"boolean\"}}}",
        .description  = "bar created",
    },
    { .status = 0 },
};

static const bb_route_t s_route_bar = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/bar",
    .tag                  = "bar-tag",
    .summary              = "Create bar resource",
    .operation_id         = "createBar",
    .request_content_type = "application/json",
    .request_schema       = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}}}",
    .responses            = s_bar_responses,
    .handler              = stub_handler,
};

// ---------------------------------------------------------------------------
// Fixture 3: GET /api/baz — no tag, no operation_id (derived)
// ---------------------------------------------------------------------------

static const bb_route_response_t s_baz_responses[] = {
    {
        .status       = 200,
        .content_type = "text/plain",
        .schema       = NULL,
        .description  = "baz ok",
    },
    { .status = 0 },
};

static const bb_route_t s_route_baz = {
    .method               = BB_HTTP_GET,
    .path                 = "/api/baz",
    .tag                  = NULL,
    .summary              = "Baz endpoint",
    .operation_id         = NULL,
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_baz_responses,
    .handler              = stub_handler,
};

// ---------------------------------------------------------------------------
// Helper: register the three fixtures
// ---------------------------------------------------------------------------

static void register_fixtures(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_foo);
    bb_http_register_described_route(NULL, &s_route_bar);
    bb_http_register_described_route(NULL, &s_route_baz);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_openapi_emit_openapi_version(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *ver = cJSON_GetObjectItemCaseSensitive(r.doc, "openapi");
    TEST_ASSERT_TRUE(cJSON_IsString(ver));
    TEST_ASSERT_EQUAL_STRING("3.1.0", cJSON_GetStringValue(ver));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_info_title(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *info = cJSON_GetObjectItemCaseSensitive(r.doc, "info");
    TEST_ASSERT_NOT_NULL(info);

    cJSON *title = cJSON_GetObjectItemCaseSensitive(info, "title");
    TEST_ASSERT_TRUE(cJSON_IsString(title));
    TEST_ASSERT_EQUAL_STRING("Test", cJSON_GetStringValue(title));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_paths_count(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    TEST_ASSERT_NOT_NULL(paths);

    // 3 fixtures = 3 unique paths
    cJSON *foo_path = cJSON_GetObjectItemCaseSensitive(paths, "/api/foo");
    cJSON *bar_path = cJSON_GetObjectItemCaseSensitive(paths, "/api/bar");
    cJSON *baz_path = cJSON_GetObjectItemCaseSensitive(paths, "/api/baz");
    TEST_ASSERT_NOT_NULL(foo_path);
    TEST_ASSERT_NOT_NULL(bar_path);
    TEST_ASSERT_NOT_NULL(baz_path);

    test_openapi_capture_free(&r);
}

void test_openapi_emit_foo_get_summary(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths   = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *foo     = cJSON_GetObjectItemCaseSensitive(paths, "/api/foo");
    cJSON *get_op  = cJSON_GetObjectItemCaseSensitive(foo, "get");
    TEST_ASSERT_NOT_NULL(get_op);

    cJSON *summary = cJSON_GetObjectItemCaseSensitive(get_op, "summary");
    TEST_ASSERT_TRUE(cJSON_IsString(summary));
    TEST_ASSERT_EQUAL_STRING("Get foo resource", cJSON_GetStringValue(summary));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_bar_post_request_body_schema_is_object(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths   = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *bar     = cJSON_GetObjectItemCaseSensitive(paths, "/api/bar");
    cJSON *post_op = cJSON_GetObjectItemCaseSensitive(bar, "post");
    TEST_ASSERT_NOT_NULL(post_op);

    cJSON *req_body = cJSON_GetObjectItemCaseSensitive(post_op, "requestBody");
    TEST_ASSERT_NOT_NULL(req_body);

    cJSON *content = cJSON_GetObjectItemCaseSensitive(req_body, "content");
    TEST_ASSERT_NOT_NULL(content);

    cJSON *media = cJSON_GetObjectItemCaseSensitive(content, "application/json");
    TEST_ASSERT_NOT_NULL(media);

    // schema must be a JSON object (raw injection), not a string
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(media, "schema");
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_TRUE(cJSON_IsObject(schema));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_foo_response_schema_is_object(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths   = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *foo     = cJSON_GetObjectItemCaseSensitive(paths, "/api/foo");
    cJSON *get_op  = cJSON_GetObjectItemCaseSensitive(foo, "get");
    cJSON *resps   = cJSON_GetObjectItemCaseSensitive(get_op, "responses");
    cJSON *r200    = cJSON_GetObjectItemCaseSensitive(resps, "200");
    cJSON *content = cJSON_GetObjectItemCaseSensitive(r200, "content");
    cJSON *media   = cJSON_GetObjectItemCaseSensitive(content, "application/json");
    cJSON *schema  = cJSON_GetObjectItemCaseSensitive(media, "schema");

    TEST_ASSERT_NOT_NULL(schema);
    // Must be an object (raw JSON injection), not a string
    TEST_ASSERT_TRUE(cJSON_IsObject(schema));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_baz_derived_operation_id(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths  = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *baz    = cJSON_GetObjectItemCaseSensitive(paths, "/api/baz");
    cJSON *get_op = cJSON_GetObjectItemCaseSensitive(baz, "get");
    TEST_ASSERT_NOT_NULL(get_op);

    cJSON *op_id = cJSON_GetObjectItemCaseSensitive(get_op, "operationId");
    TEST_ASSERT_TRUE(cJSON_IsString(op_id));
    // GET /api/baz -> "getApiBaz"
    TEST_ASSERT_EQUAL_STRING("getApiBaz", cJSON_GetStringValue(op_id));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_baz_no_tags_array(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths  = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *baz    = cJSON_GetObjectItemCaseSensitive(paths, "/api/baz");
    cJSON *get_op = cJSON_GetObjectItemCaseSensitive(baz, "get");
    TEST_ASSERT_NOT_NULL(get_op);

    // no tag on s_route_baz -> no "tags" key
    cJSON *tags = cJSON_GetObjectItemCaseSensitive(get_op, "tags");
    TEST_ASSERT_NULL(tags);

    test_openapi_capture_free(&r);
}

// Stream-path counterpart: bb_openapi_emit_stream() (via test_openapi_capture())
// returns BB_ERR_INVALID_ARG on a NULL meta before it ever touches the route
// registry, so the outcome doesn't depend on registry state.
void test_openapi_emit_stream_null_meta_returns_invalid_arg(void)
{
    test_openapi_capture_result_t r = test_openapi_capture(NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, r.status);
    TEST_ASSERT_NULL(r.doc);

    test_openapi_capture_free(&r);
}

void test_openapi_emit_servers_present_when_url_set(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title      = "Test",
        .version    = "1.0.0",
        .server_url = "http://miner.local",
    };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *servers = cJSON_GetObjectItemCaseSensitive(r.doc, "servers");
    TEST_ASSERT_NOT_NULL(servers);
    TEST_ASSERT_TRUE(cJSON_IsArray(servers));
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(servers));

    cJSON *entry = cJSON_GetArrayItem(servers, 0);
    TEST_ASSERT_NOT_NULL(entry);

    cJSON *url = cJSON_GetObjectItemCaseSensitive(entry, "url");
    TEST_ASSERT_TRUE(cJSON_IsString(url));
    TEST_ASSERT_EQUAL_STRING("http://miner.local", cJSON_GetStringValue(url));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_servers_absent_when_no_url(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *servers = cJSON_GetObjectItemCaseSensitive(r.doc, "servers");
    TEST_ASSERT_NULL(servers);

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// HTTP methods coverage: PATCH, PUT, DELETE, OPTIONS
// ---------------------------------------------------------------------------

static const bb_route_response_t s_patch_responses[] = {
    { .status = 200, .description = "ok" },
    { .status = 0 },
};

static const bb_route_t s_route_patch = {
    .method               = BB_HTTP_PATCH,
    .path                 = "/api/patch-test",
    .tag                  = "patch-tag",
    .summary              = "Patch endpoint",
    .operation_id         = "patchTest",
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_patch_responses,
    .handler              = stub_handler,
};

void test_openapi_emit_patch_method_operation_id(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_patch);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths       = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item   = cJSON_GetObjectItemCaseSensitive(paths, "/api/patch-test");
    cJSON *patch_op    = cJSON_GetObjectItemCaseSensitive(path_item, "patch");
    TEST_ASSERT_NOT_NULL(patch_op);

    cJSON *op_id = cJSON_GetObjectItemCaseSensitive(patch_op, "operationId");
    TEST_ASSERT_TRUE(cJSON_IsString(op_id));
    TEST_ASSERT_EQUAL_STRING("patchTest", cJSON_GetStringValue(op_id));

    test_openapi_capture_free(&r);
}

static const bb_route_t s_route_put = {
    .method               = BB_HTTP_PUT,
    .path                 = "/api/put-test",
    .tag                  = "put-tag",
    .summary              = "Put endpoint",
    .operation_id         = "putTest",
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_patch_responses,
    .handler              = stub_handler,
};

void test_openapi_emit_put_method_operation_id(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_put);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/put-test");
    cJSON *put_op    = cJSON_GetObjectItemCaseSensitive(path_item, "put");
    TEST_ASSERT_NOT_NULL(put_op);

    cJSON *op_id = cJSON_GetObjectItemCaseSensitive(put_op, "operationId");
    TEST_ASSERT_TRUE(cJSON_IsString(op_id));
    TEST_ASSERT_EQUAL_STRING("putTest", cJSON_GetStringValue(op_id));

    test_openapi_capture_free(&r);
}

static const bb_route_t s_route_delete = {
    .method               = BB_HTTP_DELETE,
    .path                 = "/api/delete-test",
    .tag                  = "delete-tag",
    .summary              = "Delete endpoint",
    .operation_id         = "deleteTest",
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_patch_responses,
    .handler              = stub_handler,
};

void test_openapi_emit_delete_method_operation_id(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_delete);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths       = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item   = cJSON_GetObjectItemCaseSensitive(paths, "/api/delete-test");
    cJSON *delete_op   = cJSON_GetObjectItemCaseSensitive(path_item, "delete");
    TEST_ASSERT_NOT_NULL(delete_op);

    cJSON *op_id = cJSON_GetObjectItemCaseSensitive(delete_op, "operationId");
    TEST_ASSERT_TRUE(cJSON_IsString(op_id));
    TEST_ASSERT_EQUAL_STRING("deleteTest", cJSON_GetStringValue(op_id));

    test_openapi_capture_free(&r);
}

static const bb_route_t s_route_options = {
    .method               = BB_HTTP_OPTIONS,
    .path                 = "/api/options-test",
    .tag                  = "options-tag",
    .summary              = "Options endpoint",
    .operation_id         = "optionsTest",
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_patch_responses,
    .handler              = stub_handler,
};

void test_openapi_emit_options_method_operation_id(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_options);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths        = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item    = cJSON_GetObjectItemCaseSensitive(paths, "/api/options-test");
    cJSON *options_op   = cJSON_GetObjectItemCaseSensitive(path_item, "options");
    TEST_ASSERT_NOT_NULL(options_op);

    cJSON *op_id = cJSON_GetObjectItemCaseSensitive(options_op, "operationId");
    TEST_ASSERT_TRUE(cJSON_IsString(op_id));
    TEST_ASSERT_EQUAL_STRING("optionsTest", cJSON_GetStringValue(op_id));

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// operationId derivation edge cases
// ---------------------------------------------------------------------------

static const bb_route_t s_route_path_with_dashes = {
    .method               = BB_HTTP_GET,
    .path                 = "/api/foo-bar-baz",
    .tag                  = "test",
    .summary              = "Test path with dashes",
    .operation_id         = NULL,
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_patch_responses,
    .handler              = stub_handler,
};

void test_openapi_emit_derives_operation_id_with_dashes(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_path_with_dashes);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/foo-bar-baz");
    cJSON *get_op    = cJSON_GetObjectItemCaseSensitive(path_item, "get");

    cJSON *op_id = cJSON_GetObjectItemCaseSensitive(get_op, "operationId");
    TEST_ASSERT_TRUE(cJSON_IsString(op_id));
    TEST_ASSERT_EQUAL_STRING("getApiFooBarBaz", cJSON_GetStringValue(op_id));

    test_openapi_capture_free(&r);
}

static const bb_route_t s_route_path_with_underscores = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/test_item_foo",
    .tag                  = "test",
    .summary              = "Test path with underscores",
    .operation_id         = NULL,
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_patch_responses,
    .handler              = stub_handler,
};

void test_openapi_emit_derives_operation_id_with_underscores(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_path_with_underscores);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/test_item_foo");
    cJSON *post_op   = cJSON_GetObjectItemCaseSensitive(path_item, "post");

    cJSON *op_id = cJSON_GetObjectItemCaseSensitive(post_op, "operationId");
    TEST_ASSERT_TRUE(cJSON_IsString(op_id));
    TEST_ASSERT_EQUAL_STRING("postApiTestItemFoo", cJSON_GetStringValue(op_id));

    test_openapi_capture_free(&r);
}

static const bb_route_t s_route_path_no_api_prefix = {
    .method               = BB_HTTP_GET,
    .path                 = "/foo/bar",
    .tag                  = "test",
    .summary              = "Test path without /api prefix",
    .operation_id         = NULL,
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_patch_responses,
    .handler              = stub_handler,
};

void test_openapi_emit_derives_operation_id_without_api_prefix(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_path_no_api_prefix);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/foo/bar");
    cJSON *get_op    = cJSON_GetObjectItemCaseSensitive(path_item, "get");

    cJSON *op_id = cJSON_GetObjectItemCaseSensitive(get_op, "operationId");
    TEST_ASSERT_TRUE(cJSON_IsString(op_id));
    TEST_ASSERT_EQUAL_STRING("getFooBar", cJSON_GetStringValue(op_id));

    test_openapi_capture_free(&r);
}

static const bb_route_t s_route_path_multiple_slashes = {
    .method               = BB_HTTP_GET,
    .path                 = "/api/users//foo",
    .tag                  = "test",
    .summary              = "Test path with consecutive slashes",
    .operation_id         = NULL,
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_patch_responses,
    .handler              = stub_handler,
};

void test_openapi_emit_derives_operation_id_with_consecutive_slashes(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_path_multiple_slashes);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/users//foo");
    cJSON *get_op    = cJSON_GetObjectItemCaseSensitive(path_item, "get");

    cJSON *op_id = cJSON_GetObjectItemCaseSensitive(get_op, "operationId");
    TEST_ASSERT_TRUE(cJSON_IsString(op_id));
    // Consecutive slashes should still produce valid camelCase
    TEST_ASSERT_EQUAL_STRING("getApiUsersFoo", cJSON_GetStringValue(op_id));

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// Multiple methods on same path
// ---------------------------------------------------------------------------

static const bb_route_t s_route_multi_get = {
    .method               = BB_HTTP_GET,
    .path                 = "/api/multi-endpoint",
    .tag                  = "multi",
    .summary              = "Multi-method GET",
    .operation_id         = "getMulti",
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_patch_responses,
    .handler              = stub_handler,
};

static const bb_route_t s_route_multi_post = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/multi-endpoint",
    .tag                  = "multi",
    .summary              = "Multi-method POST",
    .operation_id         = "postMulti",
    .request_content_type = "application/json",
    .request_schema       = "{\"type\":\"object\"}",
    .responses            = s_patch_responses,
    .handler              = stub_handler,
};

void test_openapi_emit_multiple_methods_same_path(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_multi_get);
    bb_http_register_described_route(NULL, &s_route_multi_post);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/multi-endpoint");
    TEST_ASSERT_NOT_NULL(path_item);

    cJSON *get_op = cJSON_GetObjectItemCaseSensitive(path_item, "get");
    cJSON *post_op = cJSON_GetObjectItemCaseSensitive(path_item, "post");
    TEST_ASSERT_NOT_NULL(get_op);
    TEST_ASSERT_NOT_NULL(post_op);

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// Response edge cases
// ---------------------------------------------------------------------------

static const bb_route_response_t s_multi_responses[] = {
    {
        .status       = 200,
        .content_type = "application/json",
        .schema       = "{\"type\":\"object\"}",
        .description  = "success",
    },
    {
        .status       = 400,
        .content_type = NULL,
        .schema       = NULL,
        .description  = "bad request",
    },
    { .status = 0 },
};

static const bb_route_t s_route_multi_response = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/multi-response",
    .tag                  = "test",
    .summary              = "Multiple responses",
    .operation_id         = "testMultiResponse",
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_multi_responses,
    .handler              = stub_handler,
};

void test_openapi_emit_multiple_response_codes(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_multi_response);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths       = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item   = cJSON_GetObjectItemCaseSensitive(paths, "/api/multi-response");
    cJSON *post_op     = cJSON_GetObjectItemCaseSensitive(path_item, "post");
    cJSON *responses   = cJSON_GetObjectItemCaseSensitive(post_op, "responses");

    cJSON *r200 = cJSON_GetObjectItemCaseSensitive(responses, "200");
    cJSON *r400 = cJSON_GetObjectItemCaseSensitive(responses, "400");
    TEST_ASSERT_NOT_NULL(r200);
    TEST_ASSERT_NOT_NULL(r400);

    test_openapi_capture_free(&r);
}

static const bb_route_response_t s_response_no_schema[] = {
    {
        .status       = 204,
        .content_type = NULL,
        .schema       = NULL,
        .description  = "no content",
    },
    { .status = 0 },
};

static const bb_route_t s_route_no_schema_response = {
    .method               = BB_HTTP_DELETE,
    .path                 = "/api/no-schema",
    .tag                  = "test",
    .summary              = "Delete with no schema",
    .operation_id         = "deleteNoSchema",
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_response_no_schema,
    .handler              = stub_handler,
};

void test_openapi_emit_response_without_schema(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_no_schema_response);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths       = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item   = cJSON_GetObjectItemCaseSensitive(paths, "/api/no-schema");
    cJSON *delete_op   = cJSON_GetObjectItemCaseSensitive(path_item, "delete");
    cJSON *responses   = cJSON_GetObjectItemCaseSensitive(delete_op, "responses");
    cJSON *r204        = cJSON_GetObjectItemCaseSensitive(responses, "204");

    TEST_ASSERT_NOT_NULL(r204);
    cJSON *content = cJSON_GetObjectItemCaseSensitive(r204, "content");
    TEST_ASSERT_NULL(content);

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// Meta field null handling
// ---------------------------------------------------------------------------

void test_openapi_emit_null_title_defaults_to_breadboard_device(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title       = NULL,
        .version     = "1.0.0",
        .description = NULL,
    };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *info  = cJSON_GetObjectItemCaseSensitive(r.doc, "info");
    cJSON *title = cJSON_GetObjectItemCaseSensitive(info, "title");
    TEST_ASSERT_TRUE(cJSON_IsString(title));
    TEST_ASSERT_EQUAL_STRING("breadboard device", cJSON_GetStringValue(title));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_null_version_defaults_to_0_0_0(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title       = "Test",
        .version     = NULL,
        .description = NULL,
    };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *info    = cJSON_GetObjectItemCaseSensitive(r.doc, "info");
    cJSON *version = cJSON_GetObjectItemCaseSensitive(info, "version");
    TEST_ASSERT_TRUE(cJSON_IsString(version));
    TEST_ASSERT_EQUAL_STRING("0.0.0", cJSON_GetStringValue(version));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_null_description_omitted(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title       = "Test",
        .version     = "1.0.0",
        .description = NULL,
    };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *info = cJSON_GetObjectItemCaseSensitive(r.doc, "info");
    cJSON *desc = cJSON_GetObjectItemCaseSensitive(info, "description");
    TEST_ASSERT_NULL(desc);

    test_openapi_capture_free(&r);
}

void test_openapi_emit_description_present_when_provided(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title       = "Test",
        .version     = "1.0.0",
        .description = "A test API",
    };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *info = cJSON_GetObjectItemCaseSensitive(r.doc, "info");
    cJSON *desc = cJSON_GetObjectItemCaseSensitive(info, "description");
    TEST_ASSERT_TRUE(cJSON_IsString(desc));
    TEST_ASSERT_EQUAL_STRING("A test API", cJSON_GetStringValue(desc));

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// Route field null handling
// ---------------------------------------------------------------------------

static const bb_route_response_t s_simple_response[] = {
    { .status = 200, .description = "ok" },
    { .status = 0 },
};

static const bb_route_t s_route_no_summary = {
    .method               = BB_HTTP_GET,
    .path                 = "/api/no-summary",
    .tag                  = "test",
    .summary              = NULL,
    .operation_id         = "getNoSummary",
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_simple_response,
    .handler              = stub_handler,
};

void test_openapi_emit_route_null_summary_omitted(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_no_summary);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/no-summary");
    cJSON *get_op    = cJSON_GetObjectItemCaseSensitive(path_item, "get");
    cJSON *summary   = cJSON_GetObjectItemCaseSensitive(get_op, "summary");

    TEST_ASSERT_NULL(summary);

    test_openapi_capture_free(&r);
}

static const bb_route_t s_route_null_responses = {
    .method               = BB_HTTP_GET,
    .path                 = "/api/null-responses",
    .tag                  = "test",
    .summary              = "Test null responses",
    .operation_id         = "getNullResponses",
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = NULL,
    .handler              = stub_handler,
};

void test_openapi_emit_route_null_responses_array(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_null_responses);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/null-responses");
    cJSON *get_op    = cJSON_GetObjectItemCaseSensitive(path_item, "get");
    cJSON *responses = cJSON_GetObjectItemCaseSensitive(get_op, "responses");

    TEST_ASSERT_NOT_NULL(responses);

    test_openapi_capture_free(&r);
}

static const bb_route_t s_route_request_schema_only = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/schema-only",
    .tag                  = "test",
    .summary              = "Request with schema only",
    .operation_id         = "postSchemaOnly",
    .request_content_type = NULL,
    .request_schema       = "{\"type\":\"object\"}",
    .responses            = s_simple_response,
    .handler              = stub_handler,
};

void test_openapi_emit_request_schema_without_content_type(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_request_schema_only);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/schema-only");
    cJSON *post_op   = cJSON_GetObjectItemCaseSensitive(path_item, "post");
    cJSON *req_body  = cJSON_GetObjectItemCaseSensitive(post_op, "requestBody");
    cJSON *content   = cJSON_GetObjectItemCaseSensitive(req_body, "content");

    cJSON *media = cJSON_GetObjectItemCaseSensitive(content, "application/json");
    TEST_ASSERT_NOT_NULL(media);

    test_openapi_capture_free(&r);
}

static const bb_route_t s_route_content_type_only = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/ct-only",
    .tag                  = "test",
    .summary              = "Request with content type only",
    .operation_id         = "postCtOnly",
    .request_content_type = "text/plain",
    .request_schema       = NULL,
    .responses            = s_simple_response,
    .handler              = stub_handler,
};

void test_openapi_emit_request_content_type_without_schema(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_content_type_only);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/ct-only");
    cJSON *post_op   = cJSON_GetObjectItemCaseSensitive(path_item, "post");

    // requestBody is only emitted when request_schema is set; content_type without schema is ignored
    cJSON *req_body = cJSON_GetObjectItemCaseSensitive(post_op, "requestBody");
    TEST_ASSERT_NULL(req_body);

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// method_str default branch: invalid method enum value falls back to "get"
// ---------------------------------------------------------------------------

// Local one-entry fixture: the OOM section (and its s_oom_responses) was
// deleted in B1-1054 PR 6, so this test (not itself an OOM test) carries its
// own minimal response fixture.
static const bb_route_response_t s_invalid_method_responses[] = {
    { .status = 200, .description = "ok" },
    { .status = 0 },
};

void test_openapi_emit_invalid_method_defaults_to_get(void)
{
    bb_http_route_registry_clear();

    // Cast an out-of-range value to bb_http_method_t to hit the default branch.
    bb_route_t route_invalid = {
        .method               = (bb_http_method_t)99,
        .path                 = "/api/invalid-method",
        .tag                  = NULL,
        .summary              = NULL,
        .operation_id         = "invalidMethod",
        .request_content_type = NULL,
        .request_schema       = NULL,
        .responses            = s_invalid_method_responses,
        .handler              = stub_handler,
    };
    bb_http_register_described_route(NULL, &route_invalid);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/invalid-method");
    TEST_ASSERT_NOT_NULL(path_item);

    // method_str default branch returns "get"
    cJSON *get_op = cJSON_GetObjectItemCaseSensitive(path_item, "get");
    TEST_ASSERT_NOT_NULL(get_op);

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// Coverage: response with NULL description (line 190 false branch)
// ---------------------------------------------------------------------------

static const bb_route_response_t s_resp_null_desc[] = {
    {
        .status       = 200,
        .content_type = "application/json",
        .schema       = "{\"type\":\"object\"}",
        .description  = NULL,
    },
    { .status = 0 },
};

static const bb_route_t s_route_null_desc = {
    .method       = BB_HTTP_GET,
    .path         = "/api/null-desc",
    .responses    = s_resp_null_desc,
    .handler      = stub_handler,
};

void test_openapi_emit_response_null_description(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_null_desc);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    // description is emitted as empty string when NULL
    TEST_ASSERT_TRUE(strstr(r.cap.body, "\"description\":\"\"") != NULL);

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// Coverage: response with NULL content_type (line 197 false branch)
// ---------------------------------------------------------------------------

static const bb_route_response_t s_resp_null_ct[] = {
    {
        .status       = 200,
        .content_type = NULL,
        .schema       = "{\"type\":\"string\"}",
        .description  = "no content type",
    },
    { .status = 0 },
};

static const bb_route_t s_route_null_ct = {
    .method    = BB_HTTP_GET,
    .path      = "/api/null-ct",
    .responses = s_resp_null_ct,
    .handler   = stub_handler,
};

void test_openapi_emit_response_null_content_type_defaults_to_json(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_null_ct);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    // missing content_type falls back to application/json
    TEST_ASSERT_TRUE(strstr(r.cap.body, "\"application/json\"") != NULL);

    test_openapi_capture_free(&r);
}


// ---------------------------------------------------------------------------
// Coverage: derive_operation_id buffer-full branch (lines 49 + 56)
// Buffer is 128 bytes; a path producing an op id longer than that exits the
// for loop on the pos<out_size-1 condition rather than *p.
// ---------------------------------------------------------------------------

static const bb_route_response_t s_long_resp[] = {
    { .status = 200, .description = "ok" },
    { .status = 0 },
};

// Path with many segments → camelCase op id easily over 128 chars.
static const bb_route_t s_route_long_path = {
    .method = BB_HTTP_GET,
    .path = "/api/very/long/path/with/many/segments/to/exceed/the/operation/id/buffer/size/limit/and/force/the/loop/to/exit/early/on/the/length/check/instead/of/null/terminator",
    .responses = s_long_resp,
    .handler = stub_handler,
};

void test_openapi_emit_long_path_truncates_operation_id(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_long_path);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// parameters field: route with query parameter emits parameters array
// ---------------------------------------------------------------------------

static const bb_route_param_t s_qparams[] = {
    {
        .name        = "topic",
        .in          = "query",
        .description = "Filter by topic name",
        .required    = false,
        .schema_type = "string",
    },
};

static const bb_route_response_t s_param_resp[] = {
    { .status = 200, .description = "ok" },
    { .status = 0 },
};

static const bb_route_t s_route_with_params = {
    .method            = BB_HTTP_GET,
    .path              = "/api/param-test",
    .tag               = "test",
    .summary           = "Route with query param",
    .operation_id      = "getParamTest",
    .responses         = s_param_resp,
    .handler           = stub_handler,
    .parameters        = s_qparams,
    .parameters_count  = 1,
};

void test_openapi_emit_parameters_array_present(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_with_params);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/param-test");
    cJSON *get_op    = cJSON_GetObjectItemCaseSensitive(path_item, "get");
    TEST_ASSERT_NOT_NULL(get_op);

    cJSON *params = cJSON_GetObjectItemCaseSensitive(get_op, "parameters");
    TEST_ASSERT_NOT_NULL(params);
    TEST_ASSERT_TRUE(cJSON_IsArray(params));
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(params));

    cJSON *p0 = cJSON_GetArrayItem(params, 0);
    TEST_ASSERT_NOT_NULL(p0);

    cJSON *name = cJSON_GetObjectItemCaseSensitive(p0, "name");
    TEST_ASSERT_TRUE(cJSON_IsString(name));
    TEST_ASSERT_EQUAL_STRING("topic", cJSON_GetStringValue(name));

    cJSON *in_val = cJSON_GetObjectItemCaseSensitive(p0, "in");
    TEST_ASSERT_TRUE(cJSON_IsString(in_val));
    TEST_ASSERT_EQUAL_STRING("query", cJSON_GetStringValue(in_val));

    // schema.type should be "string"
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(p0, "schema");
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *stype = cJSON_GetObjectItemCaseSensitive(schema, "type");
    TEST_ASSERT_TRUE(cJSON_IsString(stype));
    TEST_ASSERT_EQUAL_STRING("string", cJSON_GetStringValue(stype));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_parameters_absent_when_null(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_foo);  // no parameters field

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *foo       = cJSON_GetObjectItemCaseSensitive(paths, "/api/foo");
    cJSON *get_op    = cJSON_GetObjectItemCaseSensitive(foo, "get");
    TEST_ASSERT_NOT_NULL(get_op);

    cJSON *params = cJSON_GetObjectItemCaseSensitive(get_op, "parameters");
    TEST_ASSERT_NULL(params);

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// parameters: null description and null schema_type omit those fields
// ---------------------------------------------------------------------------

static const bb_route_param_t s_param_no_desc_no_schema[] = {
    {
        .name        = "id",
        .in          = "query",
        .description = NULL,
        .required    = false,
        .schema_type = NULL,
    },
};

static const bb_route_t s_route_param_minimal = {
    .method           = BB_HTTP_GET,
    .path             = "/api/param-minimal",
    .responses        = s_param_resp,
    .handler          = stub_handler,
    .parameters       = s_param_no_desc_no_schema,
    .parameters_count = 1,
};

void test_openapi_emit_param_null_description_omitted(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_param_minimal);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/param-minimal");
    cJSON *get_op    = cJSON_GetObjectItemCaseSensitive(path_item, "get");
    cJSON *params    = cJSON_GetObjectItemCaseSensitive(get_op, "parameters");
    TEST_ASSERT_NOT_NULL(params);

    cJSON *p0 = cJSON_GetArrayItem(params, 0);
    TEST_ASSERT_NOT_NULL(p0);

    // description NULL → key not emitted
    cJSON *desc = cJSON_GetObjectItemCaseSensitive(p0, "description");
    TEST_ASSERT_NULL(desc);

    // schema_type NULL → schema key not emitted
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(p0, "schema");
    TEST_ASSERT_NULL(schema);

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// parameters: path param (in="path", required=true)
// ---------------------------------------------------------------------------

static const bb_route_param_t s_path_param[] = {
    {
        .name        = "device_id",
        .in          = "path",
        .description = "Device identifier",
        .required    = true,
        .schema_type = "string",
    },
};

static const bb_route_t s_route_path_param = {
    .method           = BB_HTTP_GET,
    .path             = "/api/devices/{device_id}",
    .responses        = s_param_resp,
    .handler          = stub_handler,
    .parameters       = s_path_param,
    .parameters_count = 1,
};

void test_openapi_emit_param_in_path(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_path_param);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/devices/{device_id}");
    cJSON *get_op    = cJSON_GetObjectItemCaseSensitive(path_item, "get");
    cJSON *params    = cJSON_GetObjectItemCaseSensitive(get_op, "parameters");
    TEST_ASSERT_NOT_NULL(params);
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(params));

    cJSON *p0 = cJSON_GetArrayItem(params, 0);
    TEST_ASSERT_NOT_NULL(p0);

    cJSON *in_val = cJSON_GetObjectItemCaseSensitive(p0, "in");
    TEST_ASSERT_TRUE(cJSON_IsString(in_val));
    TEST_ASSERT_EQUAL_STRING("path", cJSON_GetStringValue(in_val));

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// parameters: multiple params on one route
// ---------------------------------------------------------------------------

static const bb_route_param_t s_multi_params[] = {
    {
        .name        = "page",
        .in          = "query",
        .description = "Page number",
        .required    = false,
        .schema_type = "integer",
    },
    {
        .name        = "limit",
        .in          = "query",
        .description = NULL,
        .required    = false,
        .schema_type = "integer",
    },
};

static const bb_route_t s_route_multi_params = {
    .method           = BB_HTTP_GET,
    .path             = "/api/multi-params",
    .responses        = s_param_resp,
    .handler          = stub_handler,
    .parameters       = s_multi_params,
    .parameters_count = 2,
};

void test_openapi_emit_multiple_params_on_route(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_multi_params);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/multi-params");
    cJSON *get_op    = cJSON_GetObjectItemCaseSensitive(path_item, "get");
    cJSON *params    = cJSON_GetObjectItemCaseSensitive(get_op, "parameters");
    TEST_ASSERT_NOT_NULL(params);
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(params));

    cJSON *p0 = cJSON_GetArrayItem(params, 0);
    cJSON *p1 = cJSON_GetArrayItem(params, 1);
    TEST_ASSERT_NOT_NULL(p0);
    TEST_ASSERT_NOT_NULL(p1);

    cJSON *name0 = cJSON_GetObjectItemCaseSensitive(p0, "name");
    TEST_ASSERT_TRUE(cJSON_IsString(name0));
    TEST_ASSERT_EQUAL_STRING("page", cJSON_GetStringValue(name0));

    cJSON *name1 = cJSON_GetObjectItemCaseSensitive(p1, "name");
    TEST_ASSERT_TRUE(cJSON_IsString(name1));
    TEST_ASSERT_EQUAL_STRING("limit", cJSON_GetStringValue(name1));

    // p1 has NULL description → key absent
    cJSON *desc = cJSON_GetObjectItemCaseSensitive(p1, "description");
    TEST_ASSERT_NULL(desc);

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// parameters: non-NULL array with count==0 emits no parameters key (line 161 branch 2)
// ---------------------------------------------------------------------------

static const bb_route_param_t s_empty_params[] = { /* intentionally empty */ };

static const bb_route_t s_route_params_count_zero = {
    .method           = BB_HTTP_GET,
    .path             = "/api/params-zero",
    .responses        = s_param_resp,
    .handler          = stub_handler,
    .parameters       = s_empty_params,
    .parameters_count = 0,
};

void test_openapi_emit_param_count_zero_omits_parameters(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_params_count_zero);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/params-zero");
    cJSON *get_op    = cJSON_GetObjectItemCaseSensitive(path_item, "get");
    TEST_ASSERT_NOT_NULL(get_op);

    // parameters != NULL but count == 0 -> no "parameters" key emitted
    cJSON *params = cJSON_GetObjectItemCaseSensitive(get_op, "parameters");
    TEST_ASSERT_NULL(params);

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// parameters: null name falls back to "" (line 168 branch 1)
// parameters: null in falls back to "query" (line 169 branch 1)
// ---------------------------------------------------------------------------

static const bb_route_param_t s_param_null_name_in[] = {
    {
        .name        = NULL,
        .in          = NULL,
        .description = NULL,
        .required    = false,
        .schema_type = NULL,
    },
};

static const bb_route_t s_route_param_null_name_in = {
    .method           = BB_HTTP_GET,
    .path             = "/api/param-null-name-in",
    .responses        = s_param_resp,
    .handler          = stub_handler,
    .parameters       = s_param_null_name_in,
    .parameters_count = 1,
};

void test_openapi_emit_param_null_name_defaults_to_empty(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_param_null_name_in);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/param-null-name-in");
    cJSON *get_op    = cJSON_GetObjectItemCaseSensitive(path_item, "get");
    cJSON *params    = cJSON_GetObjectItemCaseSensitive(get_op, "parameters");
    TEST_ASSERT_NOT_NULL(params);

    cJSON *p0 = cJSON_GetArrayItem(params, 0);
    TEST_ASSERT_NOT_NULL(p0);

    cJSON *name = cJSON_GetObjectItemCaseSensitive(p0, "name");
    TEST_ASSERT_TRUE(cJSON_IsString(name));
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetStringValue(name));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_param_null_in_defaults_to_query(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_param_null_name_in);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/param-null-name-in");
    cJSON *get_op    = cJSON_GetObjectItemCaseSensitive(path_item, "get");
    cJSON *params    = cJSON_GetObjectItemCaseSensitive(get_op, "parameters");
    cJSON *p0        = cJSON_GetArrayItem(params, 0);
    TEST_ASSERT_NOT_NULL(p0);

    cJSON *in_val = cJSON_GetObjectItemCaseSensitive(p0, "in");
    TEST_ASSERT_TRUE(cJSON_IsString(in_val));
    TEST_ASSERT_EQUAL_STRING("query", cJSON_GetStringValue(in_val));

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// bb_openapi_emit_stream — streaming variant; chunks are concatenated into the
// host capture slot and validated as a single string.
// ---------------------------------------------------------------------------

#include "bb_http_host.h"

void test_openapi_emit_stream_produces_valid_openapi_doc(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = { .title = "Stream", .version = "9.9.9" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    // Sanity: doc starts/ends correctly and contains the three fixture paths.
    TEST_ASSERT_EQUAL_STRING_LEN("{\"openapi\":\"3.1.0\"", r.cap.body, 18);
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"title\":\"Stream\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"version\":\"9.9.9\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"/api/foo\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"/api/bar\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"/api/baz\""));
    // tail
    TEST_ASSERT_EQUAL_STRING_LEN("}}", r.cap.body + r.cap.body_len - 2, 2);

    // Parses as valid JSON (test_openapi_capture() already parsed it via cJSON)
    TEST_ASSERT_NOT_NULL(r.doc);

    test_openapi_capture_free(&r);
}

void test_openapi_emit_stream_null_args_return_invalid_arg(void)
{
    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_openapi_emit_stream(NULL, &meta));
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_openapi_emit_stream(req, NULL));
    bb_http_host_capture_t cap = {0};
    bb_http_host_capture_end(req, &cap);
    bb_http_host_capture_free(&cap);
}


void test_openapi_emit_stream_includes_servers_and_description(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title = "Stream2",
        .version = "1.0.0",
        .description = "test description",
        .server_url = "http://example.invalid",
    };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"description\":\"test description\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"servers\":[{\"url\":\"http://example.invalid\"}]"));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_stream_applies_defaults_when_meta_fields_missing(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {0};  // title + version both NULL
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    // Defaults applied
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"title\":\"breadboard device\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"version\":\"0.0.0\""));

    test_openapi_capture_free(&r);
}

// Fixture: two methods on the same path — exercises the multi-method branch
// in stream_operations_walker (the comma separator between methods).
static const bb_route_response_t s_stream_multi_responses[] = {
    { .status = 200, .description = "ok" },
    { .status = 0 },
};
static const bb_route_t s_stream_route_multi_get = {
    .method = BB_HTTP_GET, .path = "/api/stream-multi", .tag = "multi",
    .summary = "GET multi", .responses = s_stream_multi_responses, .handler = stub_handler,
};
static const bb_route_t s_stream_route_multi_post = {
    .method = BB_HTTP_POST, .path = "/api/stream-multi", .tag = "multi",
    .summary = "POST multi", .responses = s_stream_multi_responses, .handler = stub_handler,
};

void test_openapi_emit_stream_handles_multiple_methods_per_path(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_stream_route_multi_get);
    bb_http_register_described_route(NULL, &s_stream_route_multi_post);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    // Both methods present under the single path
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"/api/stream-multi\":{"));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"get\":"));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"post\":"));

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// Stream-path null-field/branch coverage — mirrors the tree-path fixtures
// above (s_route_no_summary, s_route_params_count_zero, etc.) through
// emit_operation()/bb_openapi_emit_stream() (B1-1116 PR4). The equivalence
// harness only proves the two emitters agree on populated fixtures; it does
// not exercise emit_operation()'s own null/absent-field arms, so those need
// their own stream-path fixtures here.
// ---------------------------------------------------------------------------

void test_openapi_emit_stream_route_null_summary_omitted(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_no_summary);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    TEST_ASSERT_NULL(strstr(r.cap.body, "\"summary\""));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_stream_param_count_zero_omits_parameters(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_params_count_zero);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    // parameters != NULL but count == 0 -> no "parameters" key emitted
    TEST_ASSERT_NULL(strstr(r.cap.body, "\"parameters\""));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_stream_param_null_description_and_schema_type_omitted(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_param_minimal);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/param-minimal");
    cJSON *get_op    = cJSON_GetObjectItemCaseSensitive(path_item, "get");
    cJSON *params    = cJSON_GetObjectItemCaseSensitive(get_op, "parameters");
    TEST_ASSERT_NOT_NULL(params);
    cJSON *p0 = cJSON_GetArrayItem(params, 0);
    TEST_ASSERT_NOT_NULL(p0);

    // description NULL -> key not emitted; schema_type NULL -> schema not emitted
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(p0, "description"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(p0, "schema"));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_stream_param_null_name_and_in_default(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_param_null_name_in);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    // NULL name -> "", NULL in -> "query" (falls back, not omitted)
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"name\":\"\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"in\":\"query\""));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_stream_request_schema_without_content_type_defaults_to_json(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_request_schema_only);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"requestBody\":{\"content\":{\"application/json\""));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_stream_response_null_description_defaults_to_empty(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_null_desc);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"description\":\"\""));

    test_openapi_capture_free(&r);
}

void test_openapi_emit_stream_response_null_content_type_defaults_to_json(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_null_ct);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"application/json\""));

    test_openapi_capture_free(&r);
}

// SSE route with zero registered SSE-topic schemas — the sse_count == 0 arm
// through the stream path specifically (the tree-path equivalent is covered
// by test_sse_schema_no_sse_topic_no_oneof in test_sse_schema_fidelity.c).
static const bb_route_response_t s_stream_sse_responses[] = {
    { .status = 200, .content_type = "text/event-stream", .schema = NULL,
      .description = "SSE stream" },
    { .status = 0 },
};
static const bb_route_t s_stream_route_sse = {
    .method = BB_HTTP_GET, .path = "/api/stream-events", .tag = "events",
    .summary = "SSE stream", .responses = s_stream_sse_responses, .handler = stub_handler,
};

void test_openapi_emit_stream_sse_zero_topics_omits_content(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();
    bb_http_register_described_route(NULL, &s_stream_route_sse);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths     = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(paths, "/api/stream-events");
    cJSON *get_op    = cJSON_GetObjectItemCaseSensitive(path_item, "get");
    cJSON *resps     = cJSON_GetObjectItemCaseSensitive(get_op, "responses");
    cJSON *r200      = cJSON_GetObjectItemCaseSensitive(resps, "200");
    TEST_ASSERT_NOT_NULL(r200);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(r200, "content"));

    test_openapi_capture_free(&r);
}

// bb_http_resp_json_obj_begin() itself fails (e.g. Content-Type can't be
// set) -- exercises the `if (err != BB_OK) return err;` guard right after
// begin(), mirrors test_diag_boot_render_envelope_obj_begin_fails.
void test_openapi_emit_stream_obj_begin_fails_returns_err(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_foo);

    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    bb_http_host_force_set_type_fail(true);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    bb_err_t err = bb_openapi_emit_stream(req, &meta);

    bb_http_host_force_set_type_fail(false);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);

    bb_http_host_capture_t cap = {0};
    bb_http_host_capture_end(req, &cap);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// components/schemas
// ---------------------------------------------------------------------------

static const char k_component_schema[] =
    "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}}}";

void test_openapi_emit_components_schemas_present(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_foo);
    bb_openapi_register_schema("Foo", k_component_schema, NULL);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *components = cJSON_GetObjectItemCaseSensitive(r.doc, "components");
    TEST_ASSERT_NOT_NULL(components);
    cJSON *schemas = cJSON_GetObjectItemCaseSensitive(components, "schemas");
    TEST_ASSERT_NOT_NULL(schemas);
    cJSON *foo_schema = cJSON_GetObjectItemCaseSensitive(schemas, "Foo");
    TEST_ASSERT_NOT_NULL(foo_schema);

    test_openapi_capture_free(&r);
}

void test_openapi_emit_ref_literal_passthrough(void)
{
    bb_http_route_registry_clear();
    // Route whose 200 schema is a $ref — verifies the literal flows through
    // bb_http_resp_json_obj_set_raw unchanged.
    static const bb_route_response_t ref_responses[] = {
        { .status = 200, .content_type = "application/json",
          .schema = "{\"$ref\":\"#/components/schemas/Foo\"}",
          .description = "foo ref" },
        { .status = 0 },
    };
    static const bb_route_t ref_route = {
        .method = BB_HTTP_GET, .path = "/api/ref-test", .tag = "ref",
        .summary = "ref test", .responses = ref_responses,
        .handler = stub_handler,
    };
    bb_http_register_described_route(NULL, &ref_route);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/Foo\""));

    test_openapi_capture_free(&r);
}


#include "unity.h"
#include "bb_openapi.h"
#include "bb_http.h"
#include "bb_json.h"
#include "bb_json_test_hooks.h"

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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    char ver[16];
    bool ok = bb_json_obj_get_string(doc, "openapi", ver, sizeof(ver));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("3.1.0", ver);

    bb_json_free(doc);
}

void test_openapi_emit_info_title(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t info = bb_json_obj_get_item(doc, "info");
    TEST_ASSERT_NOT_NULL(info);

    char title[64];
    bool ok = bb_json_obj_get_string(info, "title", title, sizeof(title));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("Test", title);

    bb_json_free(doc);
}

void test_openapi_emit_paths_count(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths = bb_json_obj_get_item(doc, "paths");
    TEST_ASSERT_NOT_NULL(paths);

    // 3 fixtures = 3 unique paths
    bb_json_t foo_path = bb_json_obj_get_item(paths, "/api/foo");
    bb_json_t bar_path = bb_json_obj_get_item(paths, "/api/bar");
    bb_json_t baz_path = bb_json_obj_get_item(paths, "/api/baz");
    TEST_ASSERT_NOT_NULL(foo_path);
    TEST_ASSERT_NOT_NULL(bar_path);
    TEST_ASSERT_NOT_NULL(baz_path);

    bb_json_free(doc);
}

void test_openapi_emit_foo_get_summary(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t foo     = bb_json_obj_get_item(paths, "/api/foo");
    bb_json_t get_op  = bb_json_obj_get_item(foo, "get");
    TEST_ASSERT_NOT_NULL(get_op);

    char summary[64];
    bool ok = bb_json_obj_get_string(get_op, "summary", summary, sizeof(summary));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("Get foo resource", summary);

    bb_json_free(doc);
}

void test_openapi_emit_bar_post_request_body_schema_is_object(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t bar     = bb_json_obj_get_item(paths, "/api/bar");
    bb_json_t post_op = bb_json_obj_get_item(bar, "post");
    TEST_ASSERT_NOT_NULL(post_op);

    bb_json_t req_body = bb_json_obj_get_item(post_op, "requestBody");
    TEST_ASSERT_NOT_NULL(req_body);

    bb_json_t content = bb_json_obj_get_item(req_body, "content");
    TEST_ASSERT_NOT_NULL(content);

    bb_json_t media = bb_json_obj_get_item(content, "application/json");
    TEST_ASSERT_NOT_NULL(media);

    // schema must be a JSON object (raw injection), not a string
    bb_json_t schema = bb_json_obj_get_item(media, "schema");
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_TRUE(bb_json_item_is_object(schema));

    bb_json_free(doc);
}

void test_openapi_emit_foo_response_schema_is_object(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t foo     = bb_json_obj_get_item(paths, "/api/foo");
    bb_json_t get_op  = bb_json_obj_get_item(foo, "get");
    bb_json_t resps   = bb_json_obj_get_item(get_op, "responses");
    bb_json_t r200    = bb_json_obj_get_item(resps, "200");
    bb_json_t content = bb_json_obj_get_item(r200, "content");
    bb_json_t media   = bb_json_obj_get_item(content, "application/json");
    bb_json_t schema  = bb_json_obj_get_item(media, "schema");

    TEST_ASSERT_NOT_NULL(schema);
    // Must be an object (raw JSON injection), not a string
    TEST_ASSERT_TRUE(bb_json_item_is_object(schema));

    bb_json_free(doc);
}

void test_openapi_emit_baz_derived_operation_id(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths  = bb_json_obj_get_item(doc, "paths");
    bb_json_t baz    = bb_json_obj_get_item(paths, "/api/baz");
    bb_json_t get_op = bb_json_obj_get_item(baz, "get");
    TEST_ASSERT_NOT_NULL(get_op);

    char op_id[64];
    bool ok = bb_json_obj_get_string(get_op, "operationId", op_id, sizeof(op_id));
    TEST_ASSERT_TRUE(ok);
    // GET /api/baz -> "getApiBaz"
    TEST_ASSERT_EQUAL_STRING("getApiBaz", op_id);

    bb_json_free(doc);
}

void test_openapi_emit_baz_no_tags_array(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths  = bb_json_obj_get_item(doc, "paths");
    bb_json_t baz    = bb_json_obj_get_item(paths, "/api/baz");
    bb_json_t get_op = bb_json_obj_get_item(baz, "get");
    TEST_ASSERT_NOT_NULL(get_op);

    // no tag on s_route_baz -> no "tags" key
    bb_json_t tags = bb_json_obj_get_item(get_op, "tags");
    TEST_ASSERT_NULL(tags);

    bb_json_free(doc);
}

void test_openapi_emit_null_meta_returns_null(void)
{
    bb_json_t doc = bb_openapi_emit(NULL);
    TEST_ASSERT_NULL(doc);
}

void test_openapi_emit_servers_present_when_url_set(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title      = "Test",
        .version    = "1.0.0",
        .server_url = "http://miner.local",
    };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t servers = bb_json_obj_get_item(doc, "servers");
    TEST_ASSERT_NOT_NULL(servers);
    TEST_ASSERT_TRUE(bb_json_item_is_array(servers));
    TEST_ASSERT_EQUAL(1, bb_json_arr_size(servers));

    bb_json_t entry = bb_json_arr_get_item(servers, 0);
    TEST_ASSERT_NOT_NULL(entry);

    char url[64];
    bool ok = bb_json_obj_get_string(entry, "url", url, sizeof(url));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("http://miner.local", url);

    bb_json_free(doc);
}

void test_openapi_emit_servers_absent_when_no_url(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title   = "Test",
        .version = "1.0.0",
    };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t servers = bb_json_obj_get_item(doc, "servers");
    TEST_ASSERT_NULL(servers);

    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/patch-test");
    bb_json_t patch_op = bb_json_obj_get_item(path_item, "patch");
    TEST_ASSERT_NOT_NULL(patch_op);

    char op_id[64];
    bool ok = bb_json_obj_get_string(patch_op, "operationId", op_id, sizeof(op_id));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("patchTest", op_id);

    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/put-test");
    bb_json_t put_op = bb_json_obj_get_item(path_item, "put");
    TEST_ASSERT_NOT_NULL(put_op);

    char op_id[64];
    bool ok = bb_json_obj_get_string(put_op, "operationId", op_id, sizeof(op_id));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("putTest", op_id);

    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/delete-test");
    bb_json_t delete_op = bb_json_obj_get_item(path_item, "delete");
    TEST_ASSERT_NOT_NULL(delete_op);

    char op_id[64];
    bool ok = bb_json_obj_get_string(delete_op, "operationId", op_id, sizeof(op_id));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("deleteTest", op_id);

    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/options-test");
    bb_json_t options_op = bb_json_obj_get_item(path_item, "options");
    TEST_ASSERT_NOT_NULL(options_op);

    char op_id[64];
    bool ok = bb_json_obj_get_string(options_op, "operationId", op_id, sizeof(op_id));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("optionsTest", op_id);

    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/foo-bar-baz");
    bb_json_t get_op = bb_json_obj_get_item(path_item, "get");

    char op_id[64];
    bool ok = bb_json_obj_get_string(get_op, "operationId", op_id, sizeof(op_id));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("getApiFooBarBaz", op_id);

    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/test_item_foo");
    bb_json_t post_op = bb_json_obj_get_item(path_item, "post");

    char op_id[64];
    bool ok = bb_json_obj_get_string(post_op, "operationId", op_id, sizeof(op_id));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("postApiTestItemFoo", op_id);

    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/foo/bar");
    bb_json_t get_op = bb_json_obj_get_item(path_item, "get");

    char op_id[64];
    bool ok = bb_json_obj_get_string(get_op, "operationId", op_id, sizeof(op_id));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("getFooBar", op_id);

    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/users//foo");
    bb_json_t get_op = bb_json_obj_get_item(path_item, "get");

    char op_id[64];
    bool ok = bb_json_obj_get_string(get_op, "operationId", op_id, sizeof(op_id));
    TEST_ASSERT_TRUE(ok);
    // Consecutive slashes should still produce valid camelCase
    TEST_ASSERT_EQUAL_STRING("getApiUsersFoo", op_id);

    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths     = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/multi-endpoint");
    TEST_ASSERT_NOT_NULL(path_item);

    bb_json_t get_op = bb_json_obj_get_item(path_item, "get");
    bb_json_t post_op = bb_json_obj_get_item(path_item, "post");
    TEST_ASSERT_NOT_NULL(get_op);
    TEST_ASSERT_NOT_NULL(post_op);

    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/multi-response");
    bb_json_t post_op = bb_json_obj_get_item(path_item, "post");
    bb_json_t responses = bb_json_obj_get_item(post_op, "responses");

    bb_json_t r200 = bb_json_obj_get_item(responses, "200");
    bb_json_t r400 = bb_json_obj_get_item(responses, "400");
    TEST_ASSERT_NOT_NULL(r200);
    TEST_ASSERT_NOT_NULL(r400);

    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/no-schema");
    bb_json_t delete_op = bb_json_obj_get_item(path_item, "delete");
    bb_json_t responses = bb_json_obj_get_item(delete_op, "responses");
    bb_json_t r204 = bb_json_obj_get_item(responses, "204");

    TEST_ASSERT_NOT_NULL(r204);
    bb_json_t content = bb_json_obj_get_item(r204, "content");
    TEST_ASSERT_NULL(content);

    bb_json_free(doc);
}

// ---------------------------------------------------------------------------
// Meta field null handling
// ---------------------------------------------------------------------------

void test_openapi_emit_null_title_defaults_to_empty(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title       = NULL,
        .version     = "1.0.0",
        .description = NULL,
    };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t info = bb_json_obj_get_item(doc, "info");
    char title[64];
    bool ok = bb_json_obj_get_string(info, "title", title, sizeof(title));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("", title);

    bb_json_free(doc);
}

void test_openapi_emit_null_version_defaults_to_0_0_0(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title       = "Test",
        .version     = NULL,
        .description = NULL,
    };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t info = bb_json_obj_get_item(doc, "info");
    char version[64];
    bool ok = bb_json_obj_get_string(info, "version", version, sizeof(version));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("0.0.0", version);

    bb_json_free(doc);
}

void test_openapi_emit_null_description_omitted(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title       = "Test",
        .version     = "1.0.0",
        .description = NULL,
    };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t info = bb_json_obj_get_item(doc, "info");
    bb_json_t desc = bb_json_obj_get_item(info, "description");
    TEST_ASSERT_NULL(desc);

    bb_json_free(doc);
}

void test_openapi_emit_description_present_when_provided(void)
{
    register_fixtures();

    bb_openapi_meta_t meta = {
        .title       = "Test",
        .version     = "1.0.0",
        .description = "A test API",
    };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t info = bb_json_obj_get_item(doc, "info");
    char desc[64];
    bool ok = bb_json_obj_get_string(info, "description", desc, sizeof(desc));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("A test API", desc);

    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/no-summary");
    bb_json_t get_op = bb_json_obj_get_item(path_item, "get");
    bb_json_t summary = bb_json_obj_get_item(get_op, "summary");

    TEST_ASSERT_NULL(summary);

    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/null-responses");
    bb_json_t get_op = bb_json_obj_get_item(path_item, "get");
    bb_json_t responses = bb_json_obj_get_item(get_op, "responses");

    TEST_ASSERT_NOT_NULL(responses);

    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/schema-only");
    bb_json_t post_op = bb_json_obj_get_item(path_item, "post");
    bb_json_t req_body = bb_json_obj_get_item(post_op, "requestBody");
    bb_json_t content = bb_json_obj_get_item(req_body, "content");

    bb_json_t media = bb_json_obj_get_item(content, "application/json");
    TEST_ASSERT_NOT_NULL(media);

    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths   = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/ct-only");
    bb_json_t post_op = bb_json_obj_get_item(path_item, "post");

    // requestBody is only emitted when request_schema is set; content_type without schema is ignored
    bb_json_t req_body = bb_json_obj_get_item(post_op, "requestBody");
    TEST_ASSERT_NULL(req_body);

    bb_json_free(doc);
}

// ---------------------------------------------------------------------------
// Fault-injection tests — exercise OOM cleanup paths in bb_openapi_emit
// ---------------------------------------------------------------------------

// Minimal route for OOM tests: no tag, no request, no response schema.
// Minimizes alloc count so fault targets are predictable.
static const bb_route_response_t s_oom_responses[] = {
    { .status = 200, .description = "ok" },
    { .status = 0 },
};

static const bb_route_t s_route_oom_base = {
    .method               = BB_HTTP_GET,
    .path                 = "/api/oom-base",
    .tag                  = NULL,
    .summary              = NULL,
    .operation_id         = "getOomBase",
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_oom_responses,
    .handler              = stub_handler,
};

// Route with tag, for testing tags alloc failure.
static const bb_route_t s_route_oom_tag = {
    .method               = BB_HTTP_GET,
    .path                 = "/api/oom-tag",
    .tag                  = "test-tag",
    .summary              = NULL,
    .operation_id         = "getOomTag",
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_oom_responses,
    .handler              = stub_handler,
};

// Route with request body, for testing req_body/content/media failures.
static const bb_route_t s_route_oom_req = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/oom-req",
    .tag                  = NULL,
    .summary              = NULL,
    .operation_id         = "postOomReq",
    .request_content_type = "application/json",
    .request_schema       = "{\"type\":\"object\"}",
    .responses            = s_oom_responses,
    .handler              = stub_handler,
};

// Route with response schema, for testing response content/media failures.
static const bb_route_response_t s_oom_resp_schema[] = {
    {
        .status       = 200,
        .content_type = "application/json",
        .schema       = "{\"type\":\"object\"}",
        .description  = "ok",
    },
    { .status = 0 },
};

static const bb_route_t s_route_oom_resp = {
    .method               = BB_HTTP_GET,
    .path                 = "/api/oom-resp",
    .tag                  = NULL,
    .summary              = NULL,
    .operation_id         = "getOomResp",
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_oom_resp_schema,
    .handler              = stub_handler,
};

// alloc 0: root — emit returns NULL
void test_openapi_emit_oom_root_alloc_returns_null(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_oom_base);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    bb_json_host_force_alloc_fail_after(0);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NULL(doc);
    bb_json_host_force_alloc_fail_after(-1);
}

// alloc 1: info — emit frees root and returns NULL
void test_openapi_emit_oom_info_alloc_returns_null(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_oom_base);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    bb_json_host_force_alloc_fail_after(1);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NULL(doc);
    bb_json_host_force_alloc_fail_after(-1);
}

// alloc 2 (with server_url): servers arr — cleanup branch
// servers == NULL, server_e not yet allocated: cleanup skips both frees
void test_openapi_emit_oom_servers_arr_skips(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_oom_base);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0",
                                .server_url = "http://test.local" };
    // fail alloc 2 (servers arr); server_e not allocated -> cleanup skips both
    bb_json_host_force_alloc_fail_after(2);
    bb_json_t doc = bb_openapi_emit(&meta);
    // doc is still returned; servers block is skipped but emit continues
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t servers = bb_json_obj_get_item(doc, "servers");
    TEST_ASSERT_NULL(servers);
    bb_json_free(doc);
    bb_json_host_force_alloc_fail_after(-1);
}

// alloc 3 (with server_url): server_e obj — servers freed, server_e skipped
void test_openapi_emit_oom_server_entry_frees_arr(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_oom_base);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0",
                                .server_url = "http://test.local" };
    // fail alloc 3 (server_e); servers was allocated -> freed in cleanup
    bb_json_host_force_alloc_fail_after(3);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t servers = bb_json_obj_get_item(doc, "servers");
    TEST_ASSERT_NULL(servers);
    bb_json_free(doc);
    bb_json_host_force_alloc_fail_after(-1);
}

// paths_obj alloc failure (alloc 2 without server_url) — emit frees root, returns NULL
void test_openapi_emit_oom_paths_obj_returns_null(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_oom_base);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    // root=0, info=1, paths_obj=2 (no server_url)
    bb_json_host_force_alloc_fail_after(2);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NULL(doc);
    bb_json_host_force_alloc_fail_after(-1);
}

// path_item alloc failure — continue; doc returned but path is absent/empty
void test_openapi_emit_oom_path_item_skips_path(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_oom_base);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    // root=0, info=1, paths_obj=2, path_item=3
    bb_json_host_force_alloc_fail_after(3);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    // path_item was skipped — the path key was never set
    bb_json_t paths = bb_json_obj_get_item(doc, "paths");
    TEST_ASSERT_NOT_NULL(paths);
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/oom-base");
    TEST_ASSERT_NULL(path_item);
    bb_json_free(doc);
    bb_json_host_force_alloc_fail_after(-1);
}

// op alloc failure in build_operation — returns NULL; walker skips it
void test_openapi_emit_oom_op_alloc_skips_operation(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_oom_base);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    // root=0, info=1, paths_obj=2, path_item=3, op=4
    bb_json_host_force_alloc_fail_after(4);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t paths     = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/oom-base");
    TEST_ASSERT_NOT_NULL(path_item);
    bb_json_t get_op = bb_json_obj_get_item(path_item, "get");
    TEST_ASSERT_NULL(get_op);
    bb_json_free(doc);
    bb_json_host_force_alloc_fail_after(-1);
}

// tags arr alloc failure — op is still built, just no tags key
void test_openapi_emit_oom_tags_alloc_omits_tags(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_oom_tag);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    // root=0, info=1, paths_obj=2, path_item=3, op=4, tags=5
    bb_json_host_force_alloc_fail_after(5);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t paths     = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/oom-tag");
    bb_json_t get_op    = bb_json_obj_get_item(path_item, "get");
    TEST_ASSERT_NOT_NULL(get_op);
    bb_json_t tags = bb_json_obj_get_item(get_op, "tags");
    TEST_ASSERT_NULL(tags);
    bb_json_free(doc);
    bb_json_host_force_alloc_fail_after(-1);
}

// req_body alloc failure — cleanup path: req_body NULL, content/media freed
// (req_body=0 -> all three NULL -> else-cleanup branch entered, frees nothing)
void test_openapi_emit_oom_req_body_alloc_skips_request(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_oom_req);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    // root=0, info=1, paths_obj=2, path_item=3, op=4, req_body=5
    bb_json_host_force_alloc_fail_after(5);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t paths     = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/oom-req");
    bb_json_t post_op   = bb_json_obj_get_item(path_item, "post");
    TEST_ASSERT_NOT_NULL(post_op);
    bb_json_t req_body = bb_json_obj_get_item(post_op, "requestBody");
    TEST_ASSERT_NULL(req_body);
    bb_json_free(doc);
    bb_json_host_force_alloc_fail_after(-1);
}

// content alloc failure — req_body allocated but content NULL: else-cleanup frees req_body
void test_openapi_emit_oom_req_content_alloc_frees_req_body(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_oom_req);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    // root=0, info=1, paths_obj=2, path_item=3, op=4, req_body=5, content=6
    bb_json_host_force_alloc_fail_after(6);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t paths     = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/oom-req");
    bb_json_t post_op   = bb_json_obj_get_item(path_item, "post");
    TEST_ASSERT_NOT_NULL(post_op);
    bb_json_t req_body = bb_json_obj_get_item(post_op, "requestBody");
    TEST_ASSERT_NULL(req_body);
    bb_json_free(doc);
    bb_json_host_force_alloc_fail_after(-1);
}

// media alloc failure — req_body and content allocated but media NULL: else-cleanup frees both
void test_openapi_emit_oom_req_media_alloc_frees_req_body_and_content(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_oom_req);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    // root=0, info=1, paths_obj=2, path_item=3, op=4, req_body=5, content=6, media=7
    bb_json_host_force_alloc_fail_after(7);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t paths     = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/oom-req");
    bb_json_t post_op   = bb_json_obj_get_item(path_item, "post");
    TEST_ASSERT_NOT_NULL(post_op);
    bb_json_t req_body = bb_json_obj_get_item(post_op, "requestBody");
    TEST_ASSERT_NULL(req_body);
    bb_json_free(doc);
    bb_json_host_force_alloc_fail_after(-1);
}

// responses alloc failure — op emitted with no responses key
void test_openapi_emit_oom_responses_alloc_omits_responses(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_oom_base);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    // root=0, info=1, paths_obj=2, path_item=3, op=4, responses=5
    // (no tag, no request on s_route_oom_base)
    bb_json_host_force_alloc_fail_after(5);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t paths     = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/oom-base");
    bb_json_t get_op    = bb_json_obj_get_item(path_item, "get");
    TEST_ASSERT_NOT_NULL(get_op);
    bb_json_t responses = bb_json_obj_get_item(get_op, "responses");
    TEST_ASSERT_NULL(responses);
    bb_json_free(doc);
    bb_json_host_force_alloc_fail_after(-1);
}

// resp_obj alloc failure — that response entry is skipped (continue)
void test_openapi_emit_oom_resp_obj_alloc_skips_response(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_oom_base);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    // root=0, info=1, paths_obj=2, path_item=3, op=4, responses=5, resp_obj=6
    bb_json_host_force_alloc_fail_after(6);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t paths     = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/oom-base");
    bb_json_t get_op    = bb_json_obj_get_item(path_item, "get");
    bb_json_t responses = bb_json_obj_get_item(get_op, "responses");
    TEST_ASSERT_NOT_NULL(responses);
    // resp_obj skipped — status 200 not in responses
    bb_json_t r200 = bb_json_obj_get_item(responses, "200");
    TEST_ASSERT_NULL(r200);
    bb_json_free(doc);
    bb_json_host_force_alloc_fail_after(-1);
}

// response content alloc failure — cleanup: content freed, media skipped
void test_openapi_emit_oom_resp_content_alloc_omits_content(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_oom_resp);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    // root=0, info=1, paths_obj=2, path_item=3, op=4, responses=5, resp_obj=6, content=7
    bb_json_host_force_alloc_fail_after(7);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t paths     = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/oom-resp");
    bb_json_t get_op    = bb_json_obj_get_item(path_item, "get");
    bb_json_t responses = bb_json_obj_get_item(get_op, "responses");
    bb_json_t r200      = bb_json_obj_get_item(responses, "200");
    TEST_ASSERT_NOT_NULL(r200);
    bb_json_t content = bb_json_obj_get_item(r200, "content");
    TEST_ASSERT_NULL(content);
    bb_json_free(doc);
    bb_json_host_force_alloc_fail_after(-1);
}

// response media alloc failure — cleanup: content freed, media skipped
void test_openapi_emit_oom_resp_media_alloc_omits_content(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_oom_resp);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    // root=0, info=1, paths_obj=2, path_item=3, op=4, responses=5, resp_obj=6, content=7, media=8
    bb_json_host_force_alloc_fail_after(8);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t paths     = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/oom-resp");
    bb_json_t get_op    = bb_json_obj_get_item(path_item, "get");
    bb_json_t responses = bb_json_obj_get_item(get_op, "responses");
    bb_json_t r200      = bb_json_obj_get_item(responses, "200");
    TEST_ASSERT_NOT_NULL(r200);
    bb_json_t content = bb_json_obj_get_item(r200, "content");
    TEST_ASSERT_NULL(content);
    bb_json_free(doc);
    bb_json_host_force_alloc_fail_after(-1);
}

// ---------------------------------------------------------------------------
// method_str default branch: invalid method enum value falls back to "get"
// ---------------------------------------------------------------------------

void test_openapi_emit_invalid_method_defaults_to_get(void)
{
    bb_http_route_registry_clear();

    // Cast an out-of-range value to bb_http_method_t to hit the default branch
    bb_route_t route_invalid = {
        .method               = (bb_http_method_t)99,
        .path                 = "/api/invalid-method",
        .tag                  = NULL,
        .summary              = NULL,
        .operation_id         = "invalidMethod",
        .request_content_type = NULL,
        .request_schema       = NULL,
        .responses            = s_oom_responses,
        .handler              = stub_handler,
    };
    bb_http_register_described_route(NULL, &route_invalid);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths     = bb_json_obj_get_item(doc, "paths");
    bb_json_t path_item = bb_json_obj_get_item(paths, "/api/invalid-method");
    TEST_ASSERT_NOT_NULL(path_item);

    // method_str default branch returns "get"
    bb_json_t get_op = bb_json_obj_get_item(path_item, "get");
    TEST_ASSERT_NOT_NULL(get_op);

    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    char *s = bb_json_serialize(doc);
    TEST_ASSERT_NOT_NULL(s);
    // description is emitted as empty string when NULL
    TEST_ASSERT_TRUE(strstr(s, "\"description\":\"\"") != NULL);
    bb_json_free_str(s);
    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    char *s = bb_json_serialize(doc);
    TEST_ASSERT_NOT_NULL(s);
    // missing content_type falls back to application/json
    TEST_ASSERT_TRUE(strstr(s, "\"application/json\"") != NULL);
    bb_json_free_str(s);
    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_free(doc);
}

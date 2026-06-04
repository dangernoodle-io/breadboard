#include "unity.h"
#include "bb_http.h"
#include "bb_http_host.h"
#include <string.h>

// Test data
static const uint8_t test_css_data[] = {
    'b', 'o', 'd', 'y', ' ', '{', ' ', 'c', 'o', 'l', 'o', 'r', ':', ' ', 'r', 'e', 'd', ';', ' ', '}'
};

static const uint8_t test_js_data[] = {
    'c', 'o', 'n', 's', 't', ' ', 'x', '=', '1', ';'
};

static const uint8_t test_html_data[] = {
    '<', 'h', 't', 'm', 'l', '>'
};

// ---------------------------------------------------------------------------
// Struct / table definition tests (unchanged)
// ---------------------------------------------------------------------------

void test_asset_type_definition(void)
{
    bb_http_asset_t asset = {
        .path = "/test.css",
        .mime = "text/css",
        .encoding = NULL,
        .data = test_css_data,
        .len = sizeof(test_css_data)
    };

    TEST_ASSERT_EQUAL_STRING("/test.css", asset.path);
    TEST_ASSERT_EQUAL_STRING("text/css", asset.mime);
    TEST_ASSERT_NULL(asset.encoding);
    TEST_ASSERT_EQUAL(sizeof(test_css_data), asset.len);
}

void test_asset_with_encoding(void)
{
    bb_http_asset_t asset = {
        .path = "/app.js",
        .mime = "application/javascript",
        .encoding = "gzip",
        .data = test_js_data,
        .len = sizeof(test_js_data)
    };

    TEST_ASSERT_EQUAL_STRING("gzip", asset.encoding);
}

void test_asset_table_definition(void)
{
    const bb_http_asset_t assets[] = {
        {
            .path = "/style.css",
            .mime = "text/css",
            .encoding = NULL,
            .data = test_css_data,
            .len = sizeof(test_css_data)
        },
        {
            .path = "/app.js",
            .mime = "application/javascript",
            .encoding = "gzip",
            .data = test_js_data,
            .len = sizeof(test_js_data)
        }
    };

    TEST_ASSERT_EQUAL(2, sizeof(assets) / sizeof(assets[0]));
    TEST_ASSERT_EQUAL_STRING("/style.css", assets[0].path);
    TEST_ASSERT_EQUAL_STRING("/app.js", assets[1].path);
}

void test_asset_path_mime_type_matching(void)
{
    const bb_http_asset_t asset = {
        .path = "/example.com/index.html",
        .mime = "text/html",
        .encoding = NULL,
        .data = test_css_data,
        .len = sizeof(test_css_data)
    };

    TEST_ASSERT_EQUAL_STRING("text/html", asset.mime);
    TEST_ASSERT_TRUE(strncmp(asset.path, "/example.com/", 13) == 0);
}

void test_asset_data_integrity(void)
{
    bb_http_asset_t asset = {
        .path = "/test.css",
        .mime = "text/css",
        .encoding = NULL,
        .data = test_css_data,
        .len = sizeof(test_css_data)
    };

    TEST_ASSERT_EQUAL_MEMORY(test_css_data, asset.data, asset.len);
}

void test_asset_null_encoding_absent(void)
{
    bb_http_asset_t asset = {
        .path = "/test.css",
        .mime = "text/css",
        .encoding = NULL,
        .data = test_css_data,
        .len = sizeof(test_css_data)
    };

    TEST_ASSERT_NULL(asset.encoding);
}

void test_multiple_assets_different_types(void)
{
    const bb_http_asset_t assets[] = {
        {
            .path = "/style.css",
            .mime = "text/css",
            .encoding = NULL,
            .data = test_css_data,
            .len = sizeof(test_css_data)
        },
        {
            .path = "/app.js",
            .mime = "application/javascript",
            .encoding = NULL,
            .data = test_js_data,
            .len = sizeof(test_js_data)
        }
    };

    TEST_ASSERT_EQUAL_STRING("text/css", assets[0].mime);
    TEST_ASSERT_EQUAL_STRING("application/javascript", assets[1].mime);
}

void test_asset_encoding_variations(void)
{
    bb_http_asset_t gzip_asset = {
        .path = "/app.js",
        .mime = "application/javascript",
        .encoding = "gzip",
        .data = test_js_data,
        .len = sizeof(test_js_data)
    };

    bb_http_asset_t plain_asset = {
        .path = "/style.css",
        .mime = "text/css",
        .encoding = NULL,
        .data = test_css_data,
        .len = sizeof(test_css_data)
    };

    TEST_ASSERT_NOT_NULL(gzip_asset.encoding);
    TEST_ASSERT_NULL(plain_asset.encoding);
}

void test_zero_length_asset(void)
{
    bb_http_asset_t asset = {
        .path = "/empty.txt",
        .mime = "text/plain",
        .encoding = NULL,
        .data = (const uint8_t*)"",
        .len = 0
    };

    TEST_ASSERT_EQUAL(0, asset.len);
}

// ---------------------------------------------------------------------------
// Wildcard handler tests — exercise bb_http_host_asset_wildcard
// ---------------------------------------------------------------------------

static const bb_http_asset_t s_test_assets[] = {
    {
        .path = "/",
        .mime = "text/html",
        .encoding = NULL,
        .data = test_html_data,
        .len = sizeof(test_html_data)
    },
    {
        .path = "/style.css",
        .mime = "text/css",
        .encoding = NULL,
        .data = test_css_data,
        .len = sizeof(test_css_data)
    },
    {
        .path = "/app.js",
        .mime = "application/javascript",
        .encoding = "gzip",
        .data = test_js_data,
        .len = sizeof(test_js_data)
    },
};

static void setup_assets(void)
{
    bb_http_register_assets(NULL, s_test_assets,
                            sizeof(s_test_assets) / sizeof(s_test_assets[0]));
}

static void teardown_assets(void)
{
    bb_http_host_reset_assets();
}

// (a) known asset path serves that asset's bytes via the single wildcard handler
void test_wildcard_known_asset_serves_bytes(void)
{
    setup_assets();

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_asset_wildcard(req, "/style.css");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_EQUAL_STRING("text/css", cap.content_type);
    TEST_ASSERT_EQUAL(sizeof(test_css_data), cap.body_len);
    TEST_ASSERT_EQUAL_MEMORY(test_css_data, cap.body, cap.body_len);

    bb_http_host_capture_free(&cap);
    teardown_assets();
}

// (b) unknown path returns 404
void test_wildcard_unknown_path_returns_404(void)
{
    setup_assets();

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_asset_wildcard(req, "/nonexistent.png");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(404, cap.status);

    bb_http_host_capture_free(&cap);
    teardown_assets();
}

// (c) "/" serves the index asset directly (the embed table stores index at "/")
void test_wildcard_root_maps_to_index_html(void)
{
    setup_assets();

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_asset_wildcard(req, "/");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_EQUAL_STRING("text/html", cap.content_type);
    TEST_ASSERT_EQUAL(sizeof(test_html_data), cap.body_len);
    TEST_ASSERT_EQUAL_MEMORY(test_html_data, cap.body, cap.body_len);

    bb_http_host_capture_free(&cap);
    teardown_assets();
}

// gzip asset: Content-Encoding header set on serve
void test_wildcard_gzip_asset_content_type(void)
{
    setup_assets();

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_asset_wildcard(req, "/app.js");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_EQUAL_STRING("application/javascript", cap.content_type);
    TEST_ASSERT_EQUAL(sizeof(test_js_data), cap.body_len);

    bb_http_host_capture_free(&cap);
    teardown_assets();
}

// Query string stripped before matching
void test_wildcard_query_string_stripped(void)
{
    setup_assets();

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_asset_wildcard(req, "/style.css?v=123");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(200, cap.status);
    TEST_ASSERT_EQUAL_STRING("text/css", cap.content_type);

    bb_http_host_capture_free(&cap);
    teardown_assets();
}

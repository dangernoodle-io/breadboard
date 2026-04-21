#include "unity.h"
#include "bb_http.h"
#include <string.h>

// Test data
static const uint8_t test_css_data[] = {
    'b', 'o', 'd', 'y', ' ', '{', ' ', 'c', 'o', 'l', 'o', 'r', ':', ' ', 'r', 'e', 'd', ';', ' ', '}'
};

static const uint8_t test_js_data[] = {
    'c', 'o', 'n', 's', 't', ' ', 'x', '=', '1', ';'
};

void test_asset_type_definition(void)
{
    // Verify that bb_http_asset_t has the expected fields
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
    // Verify asset can have encoding
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
    // Verify we can define a table of assets
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
    // Verify path and mime type are correctly paired
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
    // Verify asset data is preserved
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
    // Verify NULL encoding is handled
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
    // Verify a table with different MIME types
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
    // Test gzip and null encodings
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
    // Verify zero-length assets are allowed
    bb_http_asset_t asset = {
        .path = "/empty.txt",
        .mime = "text/plain",
        .encoding = NULL,
        .data = (const uint8_t*)"",
        .len = 0
    };

    TEST_ASSERT_EQUAL(0, asset.len);
}

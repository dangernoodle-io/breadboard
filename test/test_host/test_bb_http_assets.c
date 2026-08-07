#include "unity.h"
#include "bb_http.h"
#include "bb_http_server.h"
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
    bb_http_register_assets_cfg_t cfg = {
        .assets = s_test_assets,
        .n      = sizeof(s_test_assets) / sizeof(s_test_assets[0]),
    };
    bb_http_register_assets(NULL, &cfg);
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

// ---------------------------------------------------------------------------
// bb_http_register_assets() cfg->fallback tests (B1-1458: collapsed onto a
// single cfg-struct entry point; formerly a
// bb_http_register_assets()/bb_http_register_assets_with_fallback() ladder).
//
// Note: the espidf backend's "state mutated before validation" failure path
// (httpd_register_uri_handler rejecting the wildcard route, e.g. because a
// prior handler is still registered) has no host equivalent — the host
// backend has no httpd_register_uri_handler analogue and its registration
// call cannot itself fail once entry validation passes. That failure path is
// espidf-only and is not exercised here.
// ---------------------------------------------------------------------------

static int s_fallback_calls = 0;

static bb_err_t test_fallback_handler(bb_http_request_t *req)
{
    s_fallback_calls++;
    bb_http_resp_set_status(req, 302);
    return BB_OK;
}

// (a) no-match request invokes the fallback
void test_register_assets_fallback_no_match_invokes_fallback(void)
{
    s_fallback_calls = 0;
    bb_http_register_assets_cfg_t cfg = {
        .assets   = s_test_assets,
        .n        = sizeof(s_test_assets) / sizeof(s_test_assets[0]),
        .fallback = test_fallback_handler,
    };
    bb_http_register_assets(NULL, &cfg);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_asset_wildcard(req, "/captive.apple.com/generate_204");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(1, s_fallback_calls);
    TEST_ASSERT_EQUAL(302, cap.status);

    bb_http_host_capture_free(&cap);
    teardown_assets();
}

// (b) a matched asset is served and the fallback is NOT invoked
void test_register_assets_fallback_match_does_not_invoke_fallback(void)
{
    s_fallback_calls = 0;
    bb_http_register_assets_cfg_t cfg = {
        .assets   = s_test_assets,
        .n        = sizeof(s_test_assets) / sizeof(s_test_assets[0]),
        .fallback = test_fallback_handler,
    };
    bb_http_register_assets(NULL, &cfg);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_http_host_asset_wildcard(req, "/style.css");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(0, s_fallback_calls);
    TEST_ASSERT_EQUAL(200, cap.status);

    bb_http_host_capture_free(&cap);
    teardown_assets();
}

// (c) omitting cfg->fallback (zero-init) preserves the existing 404 behavior
// -- proves zero-init reproduces the pre-collapse base bb_http_register_
// assets()'s no-fallback default exactly (the zero-trap this field is
// documented against in bb_http_server.h).
void test_register_assets_fallback_omitted_preserves_404(void)
{
    bb_http_register_assets_cfg_t cfg = {
        .assets = s_test_assets,
        .n      = sizeof(s_test_assets) / sizeof(s_test_assets[0]),
    };
    bb_http_register_assets(NULL, &cfg);

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

// (d) assets == NULL with n == 0 is a valid fallback-only registration
void test_register_assets_null_assets_n_zero_with_fallback_ok(void)
{
    s_fallback_calls = 0;
    bb_http_register_assets_cfg_t cfg = { .assets = NULL, .n = 0, .fallback = test_fallback_handler };
    bb_err_t err = bb_http_register_assets(NULL, &cfg);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t herr = bb_http_host_asset_wildcard(req, "/anything");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, herr);
    TEST_ASSERT_EQUAL(1, s_fallback_calls);

    bb_http_host_capture_free(&cap);
    teardown_assets();
}

// (d2) assets == NULL with n == 0 and no fallback is a legal no-op
// registration (an empty wildcard that always 404s) -- this closes the
// pre-collapse host/espidf divergence where the espidf backend's OLD base
// bb_http_register_assets() unconditionally rejected NULL assets even at
// n == 0, while the host backend's base wrapper did not; the collapsed
// entry point has one rule (assets == NULL legal iff n == 0) shared by both
// backends. No production caller exercises this combination.
void test_register_assets_null_assets_n_zero_no_fallback_ok(void)
{
    bb_http_register_assets_cfg_t cfg = { .assets = NULL, .n = 0 };
    bb_err_t err = bb_http_register_assets(NULL, &cfg);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_http_request_t *req;
    bb_http_host_capture_t cap;
    bb_http_host_capture_begin(&req);
    bb_err_t herr = bb_http_host_asset_wildcard(req, "/anything");
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, herr);
    TEST_ASSERT_EQUAL(404, cap.status);

    bb_http_host_capture_free(&cap);
    teardown_assets();
}

// (e) assets == NULL with n > 0 is a mismatched/invalid call
void test_register_assets_null_assets_n_nonzero_invalid_arg(void)
{
    bb_http_register_assets_cfg_t cfg = { .assets = NULL, .n = 3, .fallback = test_fallback_handler };
    bb_err_t err = bb_http_register_assets(NULL, &cfg);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

// (e2) cfg == NULL is rejected -- the collapsed entry point's own new
// validation (no equivalent existed in the pre-collapse ladder, since
// neither prior function took a struct pointer).
void test_register_assets_null_cfg_invalid_arg(void)
{
    bb_err_t err = bb_http_register_assets(NULL, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

// (f) a table entry with a NULL path/mime/data is rejected — matches the
// espidf backend's per-entry validation (regression coverage for the
// host/espidf divergence found in review).
void test_register_assets_entry_with_null_field_rejected(void)
{
    const bb_http_asset_t bad_assets[] = {
        {
            .path = NULL,
            .mime = "text/css",
            .encoding = NULL,
            .data = test_css_data,
            .len = sizeof(test_css_data)
        },
    };

    bb_http_register_assets_cfg_t cfg = {
        .assets   = bad_assets,
        .n        = sizeof(bad_assets) / sizeof(bad_assets[0]),
        .fallback = test_fallback_handler,
    };
    bb_err_t err = bb_http_register_assets(NULL, &cfg);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

// (g) a table entry with a NULL mime is rejected (covers branch arc for mime check)
void test_register_assets_entry_with_null_mime_rejected(void)
{
    const bb_http_asset_t bad_assets[] = {
        {
            .path = "/test.css",
            .mime = NULL,
            .encoding = NULL,
            .data = test_css_data,
            .len = sizeof(test_css_data)
        },
    };

    bb_http_register_assets_cfg_t cfg = {
        .assets   = bad_assets,
        .n        = sizeof(bad_assets) / sizeof(bad_assets[0]),
        .fallback = test_fallback_handler,
    };
    bb_err_t err = bb_http_register_assets(NULL, &cfg);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

// (h) a table entry with a NULL data is rejected (covers branch arc for data check)
void test_register_assets_entry_with_null_data_rejected(void)
{
    const bb_http_asset_t bad_assets[] = {
        {
            .path = "/test.css",
            .mime = "text/css",
            .encoding = NULL,
            .data = NULL,
            .len = 0
        },
    };

    bb_http_register_assets_cfg_t cfg = {
        .assets   = bad_assets,
        .n        = sizeof(bad_assets) / sizeof(bad_assets[0]),
        .fallback = test_fallback_handler,
    };
    bb_err_t err = bb_http_register_assets(NULL, &cfg);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
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

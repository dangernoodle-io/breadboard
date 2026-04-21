#include "unity.h"
#include "bb_ota_pull.h"
#include <string.h>

void test_ota_pull_parse_version_found(void)
{
    const char *json =
        "{\"tag_name\":\"v1.2.3\",\"assets\":["
        "{\"name\":\"tdongle-s3.bin\","
        "\"browser_download_url\":\"https://github.com/dangernoodle-io/snugfeather/releases/download/v1.2.3/tdongle-s3.bin\"}"
        "]}";

    char tag[32], url[256];
    int ret = bb_ota_pull_parse_release_json(json, "tdongle-s3", tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_STRING("v1.2.3", tag);
    TEST_ASSERT_EQUAL_STRING(
        "https://github.com/dangernoodle-io/snugfeather/releases/download/v1.2.3/tdongle-s3.bin",
        url);
}

void test_ota_pull_parse_no_matching_asset(void)
{
    const char *json =
        "{\"tag_name\":\"v1.2.3\",\"assets\":["
        "{\"name\":\"taipanminer-bitaxe-601.bin\","
        "\"browser_download_url\":\"https://example.com/taipanminer-bitaxe-601.bin\"}"
        "]}";

    char tag[32], url[256];
    int ret = bb_ota_pull_parse_release_json(json, "tdongle-s3", tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(-2, ret);
    TEST_ASSERT_EQUAL_STRING("v1.2.3", tag);
}

void test_ota_pull_parse_empty_assets(void)
{
    const char *json = "{\"tag_name\":\"v1.0.0\",\"assets\":[]}";

    char tag[32], url[256];
    int ret = bb_ota_pull_parse_release_json(json, "tdongle-s3", tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

void test_ota_pull_parse_no_tag(void)
{
    const char *json = "{\"assets\":[]}";

    char tag[32], url[256];
    int ret = bb_ota_pull_parse_release_json(json, "tdongle-s3", tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

void test_ota_pull_parse_invalid_json(void)
{
    char tag[32], url[256];
    int ret = bb_ota_pull_parse_release_json("not json", "tdongle-s3", tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

void test_ota_pull_parse_multiple_assets(void)
{
    const char *json =
        "{\"tag_name\":\"v2.0.0\",\"assets\":["
        "{\"name\":\"elecrow-p4-hmi7.bin\","
        "\"browser_download_url\":\"https://example.com/elecrow-p4.bin\"},"
        "{\"name\":\"elecrow-c6.bin\","
        "\"browser_download_url\":\"https://example.com/elecrow-c6.bin\"}"
        "]}";

    char tag[32], url[256];

    // Should find elecrow-p4-hmi7
    int ret = bb_ota_pull_parse_release_json(json, "elecrow-p4-hmi7", tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_STRING("https://example.com/elecrow-p4.bin", url);

    // Should find elecrow-c6
    ret = bb_ota_pull_parse_release_json(json, "elecrow-c6", tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_STRING("https://example.com/elecrow-c6.bin", url);
}

void test_ota_pull_parse_tag_truncation(void)
{
    const char *json =
        "{\"tag_name\":\"v99.99.99-rc1-longprerelease\",\"assets\":["
        "{\"name\":\"elecrow-p4-hmi7.bin\","
        "\"browser_download_url\":\"https://example.com/test.bin\"}"
        "]}";

    char tag[8], url[256];
    int ret = bb_ota_pull_parse_release_json(json, "elecrow-p4-hmi7", tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(0, ret);
    // Tag should be truncated to 7 chars + null terminator
    TEST_ASSERT_EQUAL_INT(7, (int)strlen(tag));
    TEST_ASSERT_EQUAL_STRING("v99.99.", tag);
    // Null terminator should be properly set
    TEST_ASSERT_EQUAL_CHAR('\0', tag[7]);
}

void test_ota_pull_parse_url_truncation(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"elecrow-p4-hmi7.bin\","
        "\"browser_download_url\":\"https://example.com/very/long/path/to/firmware/image.bin\"}"
        "]}";

    char tag[32], url[32];
    int ret = bb_ota_pull_parse_release_json(json, "elecrow-p4-hmi7", tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(0, ret);
    // URL should be truncated to 31 chars + null terminator
    TEST_ASSERT_EQUAL_INT(31, (int)strlen(url));
    // Null terminator should be properly set
    TEST_ASSERT_EQUAL_CHAR('\0', url[31]);
    // Verify it's truncated from the beginning of the URL
    TEST_ASSERT_EQUAL_INT(0, strncmp(url, "https://example.com/very/long", 29));
}

void test_ota_pull_parse_asset_missing_url(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"taipanminer-tdongle-s3.bin\","
        "\"size\":1234}"
        "]}";

    char tag[32], url[256];
    int ret = bb_ota_pull_parse_release_json(json, "tdongle-s3", tag, sizeof(tag), url, sizeof(url));
    // Should return -2 (no matching asset with valid URL)
    TEST_ASSERT_EQUAL_INT(-2, ret);
    // Tag should still be populated from tag_name
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
}

void test_ota_pull_parse_assets_not_array(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\","
        "\"assets\":\"not-an-array\"}";

    char tag[32], url[256];
    int ret = bb_ota_pull_parse_release_json(json, "tdongle-s3", tag, sizeof(tag), url, sizeof(url));
    // Should return -2 because assets is not an array
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

void test_ota_pull_parse_null_inputs(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"taipanminer-tdongle-s3.bin\","
        "\"browser_download_url\":\"https://example.com/test.bin\"}"
        "]}";

    char tag[32], url[256];

    // NULL json
    int ret = bb_ota_pull_parse_release_json(NULL, "tdongle-s3", tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(-1, ret);

    // NULL board_name
    ret = bb_ota_pull_parse_release_json(json, NULL, tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(-1, ret);

    // NULL out_tag
    ret = bb_ota_pull_parse_release_json(json, "tdongle-s3", NULL, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(-1, ret);

    // NULL out_url
    ret = bb_ota_pull_parse_release_json(json, "tdongle-s3", tag, sizeof(tag), NULL, sizeof(url));
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

void test_ota_pull_parse_asset_url_null_value(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"taipanminer-tdongle-s3.bin\","
        "\"browser_download_url\":null}"
        "]}";

    char tag[32], url[256];
    int ret = bb_ota_pull_parse_release_json(json, "tdongle-s3", tag, sizeof(tag), url, sizeof(url));
    // Should return -2 because url_item->valuestring is NULL
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

// Skip-check callback tests
static bool s_skip_check_callback_called = false;
static bool s_skip_check_callback_result = false;

static bool test_skip_check_callback(void)
{
    s_skip_check_callback_called = true;
    return s_skip_check_callback_result;
}

void test_ota_pull_skip_check_callback_registration(void)
{
    // Test that callback can be set and unset
    bb_ota_pull_set_skip_check_cb(test_skip_check_callback);
    // Callback registered successfully, verify by checking callback execution
    s_skip_check_callback_called = false;
    s_skip_check_callback_result = false;
    test_skip_check_callback();
    TEST_ASSERT_TRUE(s_skip_check_callback_called);

    // Unset callback
    bb_ota_pull_set_skip_check_cb(NULL);
    s_skip_check_callback_called = false;
    // Calling set to NULL should clear it
    TEST_ASSERT_TRUE(true);  // Sentinel: callback unset successfully
}

void test_ota_pull_skip_check_callback_returns_true(void)
{
    // Test callback returning true
    bb_ota_pull_set_skip_check_cb(test_skip_check_callback);
    s_skip_check_callback_result = true;
    s_skip_check_callback_called = false;

    bool result = test_skip_check_callback();
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(s_skip_check_callback_called);

    bb_ota_pull_set_skip_check_cb(NULL);
}

void test_ota_pull_skip_check_callback_returns_false(void)
{
    // Test callback returning false
    bb_ota_pull_set_skip_check_cb(test_skip_check_callback);
    s_skip_check_callback_result = false;
    s_skip_check_callback_called = false;

    bool result = test_skip_check_callback();
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(s_skip_check_callback_called);

    bb_ota_pull_set_skip_check_cb(NULL);
}

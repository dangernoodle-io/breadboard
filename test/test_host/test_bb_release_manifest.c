#include "unity.h"
#include "bb_release_manifest.h"
#include <string.h>

/* Whitespace around `:` and between key/value — exercises whitespace-skip
 * branches in find_key (lines 25, 30). */
void test_bb_release_manifest_parse_github_whitespace_around_colons(void)
{
    const char *json =
        "{ \"tag_name\" : \"v1.0.0\" , \"assets\" : ["
        "{ \"name\" : \"x.bin\" , \"browser_download_url\" : \"https://example.com/x.bin\" }"
        "] }";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "x",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/x.bin", url);
}

/* Escape sequences (\\, \", \n) inside string values — exercises escape
 * branches in find_key + copy_string_value (lines 17,18,52-77). */
void test_bb_release_manifest_parse_github_handles_escape_sequences(void)
{
    /* Tag with embedded escape that mimics realistic GitHub asset names */
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"foo\\\\bar.bin\","
        "\"browser_download_url\":\"https://example.com/foo\\nbar\"}"
        "]}";
    char tag[32], url[256];
    /* Use the literal escape pattern in board name so the asset matches. */
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json),
                                                    "foo\\bar",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
}

/* Bare \uXXXX inside a string — exercises the \u skip path. */
void test_bb_release_manifest_parse_github_unicode_escape_skipped(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"a\\u0042c.bin\",\"browser_download_url\":\"https://x.com/y\"}"
        "]}";
    /* The matcher compares verbatim post-escape; B becomes empty in our
     * skip impl, so the parsed name is "ac.bin". */
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "ac",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
}

/* Multiple assets where the first does NOT match — exercises asset-loop
 * iteration (lines 150-172). */
void test_bb_release_manifest_parse_github_skips_non_matching_first_asset(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"other.bin\",\"browser_download_url\":\"https://x/other.bin\"},"
        "{\"name\":\"target.bin\",\"browser_download_url\":\"https://x/target.bin\"}"
        "]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "target",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("https://x/target.bin", url);
}

/* Asset object with no `name` field — exercises the "name not found" branch
 * inside the loop (line 161 false path). */
void test_bb_release_manifest_parse_github_asset_missing_name_skipped(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"size\":42},"
        "{\"name\":\"target.bin\",\"browser_download_url\":\"https://x/target.bin\"}"
        "]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "target",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
}

/* Matching asset but the `browser_download_url` field is missing — exercises
 * the url-not-found return inside the matching branch (line 167). */
void test_bb_release_manifest_parse_github_matching_asset_no_url(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"target.bin\",\"size\":42}"
        "]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "target",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, ret);
    /* Tag was extracted before the failure. */
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
}

/* assets value is not an array — exercises the assets-not-array branch (143). */
void test_bb_release_manifest_parse_github_assets_not_array(void)
{
    const char *json = "{\"tag_name\":\"v1.0.0\",\"assets\":\"oops\"}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "x",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, ret);
}

/* Empty assets array — exercises the loop-exit branches (153 array-end, 154 not-{). */
void test_bb_release_manifest_parse_github_empty_assets_array(void)
{
    const char *json = "{\"tag_name\":\"v1.0.0\",\"assets\":[]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "x",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, ret);
}

void test_bb_release_manifest_parse_github_valid_manifest(void)
{
    const char *json =
        "{\"tag_name\":\"v1.2.3\",\"assets\":["
        "{\"name\":\"tdongle-s3.bin\","
        "\"browser_download_url\":\"https://github.com/dangernoodle-io/snugfeather/releases/download/v1.2.3/tdongle-s3.bin\"}"
        "]}";

    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(
        json, strlen(json),
        "tdongle-s3",
        tag, sizeof(tag),
        url, sizeof(url));

    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.2.3", tag);
    TEST_ASSERT_EQUAL_STRING(
        "https://github.com/dangernoodle-io/snugfeather/releases/download/v1.2.3/tdongle-s3.bin",
        url);
}

void test_bb_release_manifest_parse_github_missing_tag(void)
{
    const char *json = "{\"assets\":[]}";

    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(
        json, strlen(json),
        "tdongle-s3",
        tag, sizeof(tag),
        url, sizeof(url));

    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, ret);
}

void test_bb_release_manifest_parse_github_missing_asset(void)
{
    const char *json =
        "{\"tag_name\":\"v1.2.3\",\"assets\":["
        "{\"name\":\"taipanminer-bitaxe-601.bin\","
        "\"browser_download_url\":\"https://example.com/taipanminer-bitaxe-601.bin\"}"
        "]}";

    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(
        json, strlen(json),
        "tdongle-s3",
        tag, sizeof(tag),
        url, sizeof(url));

    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, ret);
}

void test_bb_release_manifest_parse_github_null_args(void)
{
    const char *json = "{\"tag_name\":\"v1.0\",\"assets\":[]}";
    char tag[32], url[256];

    bb_err_t ret;

    // NULL body
    ret = bb_release_manifest_parse_github(NULL, 10, "board", tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, ret);

    // NULL board_name
    ret = bb_release_manifest_parse_github(json, strlen(json), NULL, tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, ret);

    // NULL out_tag
    ret = bb_release_manifest_parse_github(json, strlen(json), "board", NULL, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, ret);

    // NULL out_url
    ret = bb_release_manifest_parse_github(json, strlen(json), "board", tag, sizeof(tag), NULL, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, ret);

    // Zero tag_size
    ret = bb_release_manifest_parse_github(json, strlen(json), "board", tag, 0, url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, ret);

    // Zero url_size
    ret = bb_release_manifest_parse_github(json, strlen(json), "board", tag, sizeof(tag), url, 0);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, ret);
}

void test_bb_release_manifest_parse_github_bad_json(void)
{
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(
        "not json", strlen("not json"),
        "tdongle-s3",
        tag, sizeof(tag),
        url, sizeof(url));

    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, ret);
}

void test_bb_release_manifest_parse_github_multiple_assets(void)
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
    bb_err_t ret = bb_release_manifest_parse_github(
        json, strlen(json),
        "elecrow-p4-hmi7",
        tag, sizeof(tag),
        url, sizeof(url));

    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("https://example.com/elecrow-p4.bin", url);

    // Should find elecrow-c6
    ret = bb_release_manifest_parse_github(
        json, strlen(json),
        "elecrow-c6",
        tag, sizeof(tag),
        url, sizeof(url));

    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("https://example.com/elecrow-c6.bin", url);
}

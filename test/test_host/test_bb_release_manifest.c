#include "unity.h"
#include "bb_release_manifest.h"
#include <stdio.h>
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

/* Matching asset but the `browser_download_url` field is missing.
 *
 * BEHAVIOR CHANGE (PR3, disclosed): the pre-scanner BUFFERED parser
 * returned BB_ERR_NOT_FOUND here, while the pre-scanner STREAMING parser
 * (bb_release_manifest_github_stream.c) already returned BB_OK with an
 * empty url_out for the exact same shape -- see
 * test_stream_asset_missing_url_returns_no_asset in
 * test_release_manifest_github_stream.c, which predates this change.
 * Reimplementing both parsers on the SAME shared sink means they can no
 * longer diverge; the streaming behavior (BB_OK, empty url -- "no asset
 * for this board" is not a parse failure) is kept as canonical since it is
 * the more useful terminal for a caller that already extracted a valid
 * tag_name. */
void test_bb_release_manifest_parse_github_matching_asset_no_url(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"target.bin\",\"size\":42}"
        "]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "target",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("", url);
}

/* assets value is not an array (a string, "oops") -- a structurally
 * invalid/broken upstream response shape, not "no asset for this board".
 * Hard-fails with BB_ERR_NOT_FOUND even though tag_name was extracted,
 * matching the pre-scanner buffered parser's dedicated "assets key present
 * but not an array" check -- this is a real alerting signal
 * (bb_ota_check_common.c maps BB_ERR_NOT_FOUND to
 * BB_OTA_CHECK_OUTCOME_FAILED/last_check_ok=false) that must not be folded
 * into the same benign terminal as a missing/empty assets array. */
void test_bb_release_manifest_parse_github_assets_not_array(void)
{
    const char *json = "{\"tag_name\":\"v1.0.0\",\"assets\":\"oops\"}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "x",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, ret);
}

/* assets value is an OBJECT rather than an array -- same hard-fail as the
 * string-scalar case above, exercised via sink_begin_obj()'s
 * assets_invalid_shape detection instead of the scalar-callback path. */
void test_bb_release_manifest_parse_github_assets_is_object_not_array(void)
{
    const char *json = "{\"tag_name\":\"v1.0.0\",\"assets\":{\"oops\":1}}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "x",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, ret);
}

/* assets value is a bare number -- exercises sink_value_num()'s
 * assets_invalid_shape detection. */
void test_bb_release_manifest_parse_github_assets_is_number_not_array(void)
{
    const char *json = "{\"tag_name\":\"v1.0.0\",\"assets\":42}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "x",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, ret);
}

/* assets value is a bare null -- exercises sink_value_null()'s
 * assets_invalid_shape detection. */
void test_bb_release_manifest_parse_github_assets_is_null_not_array(void)
{
    const char *json = "{\"tag_name\":\"v1.0.0\",\"assets\":null}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "x",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, ret);
}

/* Empty assets array — exercises the loop-exit branches (153 array-end, 154 not-{).
 * tag_name found, no matching asset -> BB_OK with empty url (no-asset terminal). */
void test_bb_release_manifest_parse_github_empty_assets_array(void)
{
    const char *json = "{\"tag_name\":\"v1.0.0\",\"assets\":[]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "x",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("", url);
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
    // tag_name found, assets array present, but no asset matches the board name.
    // Returns BB_OK with empty url_out (no-asset terminal, not a parse error).
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

    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.2.3", tag);
    TEST_ASSERT_EQUAL_STRING("", url);
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

/* ---------------------------------------------------------------------
 * bb_serialize_json scanner sink parity coverage (PR3: reimplemented on
 * top of bb_serialize_json_scan_bounded()). These exercise cases the
 * old hand-rolled find_key()/copy_string_value() implementation could
 * not: escaped `\/` (extremely common in real GitHub JSON), a matching
 * asset AFTER several non-matching ones, and early-stop.
 * --------------------------------------------------------------------- */

/* \/ escape in the URL -- proves the SCANNER_SCRATCH decoded-escape copy
 * path works in bounded mode, not just \\ and bare \uXXXX. */
void test_bb_release_manifest_parse_github_slash_escape_in_url(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https:\\/\\/example.com\\/fw.bin\"}"
        "]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

/* Matching asset appears after several non-matching ones. */
void test_bb_release_manifest_parse_github_matching_asset_after_several(void)
{
    const char *json =
        "{\"tag_name\":\"v3.0.0\",\"assets\":["
        "{\"name\":\"a.bin\",\"browser_download_url\":\"https://x/a.bin\"},"
        "{\"name\":\"b.bin\",\"browser_download_url\":\"https://x/b.bin\"},"
        "{\"name\":\"c.bin\",\"browser_download_url\":\"https://x/c.bin\"},"
        "{\"name\":\"target.bin\",\"browser_download_url\":\"https://x/target.bin\"}"
        "]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "target",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v3.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://x/target.bin", url);
}

/* Early-stop: a huge trailing tail after the matching asset must not be
 * fully scanned -- an unterminated/malformed tail would fail the scan if
 * the scanner kept going, but since both fields are already found the
 * scan aborts before ever reaching it, so the (deliberately broken) tail
 * has no effect on the result. */
void test_bb_release_manifest_parse_github_early_stop_ignores_broken_tail(void)
{
    char json[4096];
    int n = snprintf(json, sizeof(json),
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"fw.bin\",\"browser_download_url\":\"https://x/fw.bin\"}"
        "],\"trailing\":\"this string is never closed and the document is truncated so a full scan would fail: ");
    /* Deliberately no closing quote/brace within the buffer -- if the
     * scanner did not stop early it would hit end-of-buffer mid-string
     * and bb_serialize_json_scan_bounded() would report a grammar error
     * (folded to BB_ERR_NOT_FOUND by finalize()), NOT BB_OK. */
    TEST_ASSERT_GREATER_THAN(0, n);
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://x/fw.bin", url);
}

/* Malformed JSON mid-stream, discovered only after tag_name was already
 * extracted -- exercises the "genuine scan error hard-fails, even after
 * tag_found" path in bb_release_manifest_sink_finalize(). A dropped/corrupt
 * fetch must not be indistinguishable from a well-formed manifest with no
 * matching asset -- see the finalize()/sink header comments. */
void test_bb_release_manifest_parse_github_malformed_after_tag_found(void)
{
    const char *json = "{\"tag_name\":\"v1.0.0\",\"assets\":[}}}not valid";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, ret);
    TEST_ASSERT_EQUAL_STRING("", url);
}

/* ---------------------------------------------------------------------
 * Full branch coverage of the shared bb_release_manifest_json_sink.c
 * sink (bounded mode; the sink is identical for both entry points, so
 * these also cover the streaming path's use of the same code).
 * --------------------------------------------------------------------- */

/* tag_out too small to hold the extracted tag -> BB_ERR_NO_SPACE. */
void test_bb_release_manifest_parse_github_tag_buffer_too_small(void)
{
    const char *json = "{\"tag_name\":\"v1.0.0\",\"assets\":[]}";
    char tag[3], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, ret);
}

/* url_out too small to hold the extracted URL -> BB_ERR_NO_SPACE. */
void test_bb_release_manifest_parse_github_url_buffer_too_small(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"fw.bin\",\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32], url[5];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, ret);
}

/* An "assets" element that is itself an ARRAY (not an object) -- exercises
 * the sink_begin_obj() depth-mismatch branch: in_assets && !in_asset_obj
 * is true, but the nested object inside the inner array sits one depth
 * level deeper than assets_elem_depth, so it is correctly NOT treated as
 * an asset element. */
void test_bb_release_manifest_parse_github_assets_element_is_array_no_match(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "[{\"name\":\"fw.bin\",\"browser_download_url\":\"https://x/fw.bin\"}]"
        "]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("", url);
}

/* A top-level array key with the SAME length as "assets" (6 bytes) but
 * different content -- exercises the memcmp()==0 false sub-branch in
 * sink_begin_arr(). */
void test_bb_release_manifest_parse_github_toplevel_six_char_key_not_assets(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"extras\":[1,2,3],\"assets\":["
        "{\"name\":\"fw.bin\",\"browser_download_url\":\"https://x/fw.bin\"}"
        "]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://x/fw.bin", url);
}

/* Duplicate top-level "tag_name" key -- the second occurrence exercises
 * the !tag_found false sub-branch in sink_value_str_chunk()'s is_tag
 * check (the first occurrence already set tag_found, so the duplicate is
 * ignored -- the first value wins). */
void test_bb_release_manifest_parse_github_duplicate_tag_name_second_ignored(void)
{
    const char *json = "{\"tag_name\":\"v1.0.0\",\"tag_name\":\"v2.0.0\",\"assets\":[]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
}

/* A top-level key with the SAME length as "tag_name" (8 bytes) but
 * different content -- exercises the memcmp()==0 false sub-branch of
 * is_tag. */
void test_bb_release_manifest_parse_github_toplevel_eight_char_key_not_tag_name(void)
{
    const char *json =
        "{\"html_url\":\"https://github.com/x\",\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"fw.bin\",\"browser_download_url\":\"https://x/fw.bin\"}"
        "]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://x/fw.bin", url);
}

/* A key literally named "name" nested one level DEEPER than the asset
 * object itself -- exercises the depth-mismatch false sub-branch of
 * is_name (s->depth == s->asset_obj_depth + 1). */
void test_bb_release_manifest_parse_github_nested_name_key_wrong_depth_ignored(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"uploader\":{\"name\":\"botname\"},"
        "\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://x/fw.bin\"}"
        "]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://x/fw.bin", url);
}

/* Duplicate matching "name" key inside the same asset object -- the
 * second occurrence exercises the !asset_name_matched false sub-branch
 * (the first occurrence already matched, so the duplicate is ignored). */
void test_bb_release_manifest_parse_github_duplicate_name_key_second_ignored(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"fw.bin\",\"name\":\"other.bin\","
        "\"browser_download_url\":\"https://x/fw.bin\"}"
        "]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://x/fw.bin", url);
}

/* A 4-char asset-object key that is NOT "name" -- exercises the
 * memcmp()==0 false sub-branch of is_name. */
void test_bb_release_manifest_parse_github_asset_four_char_key_not_name(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"etag\":\"abcd\",\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://x/fw.bin\"}"
        "]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://x/fw.bin", url);
}

/* A 20-char asset-object key that is NOT "browser_download_url", inside a
 * MATCHED asset -- exercises the memcmp()==0 false sub-branch of is_url
 * (mirrors test_..._asset_four_char_key_not_name's coverage of is_name's
 * equivalent memcmp; is_url's had no such test and was wrongly excluded
 * via LCOV_EXCL_BR_LINE -- see bb_release_manifest_json_sink.c). */
void test_bb_release_manifest_parse_github_asset_twenty_char_key_not_url(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"fw.bin\",\"aaaaaaaaaaaaaaaaaaaa\":\"decoy\","
        "\"browser_download_url\":\"https://x/fw.bin\"}"
        "]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://x/fw.bin", url);
}

/* A key literally named "browser_download_url" nested one level DEEPER
 * than the asset object -- exercises the depth-mismatch false sub-branch
 * of is_url. */
void test_bb_release_manifest_parse_github_nested_url_key_wrong_depth_ignored(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"meta\":{\"browser_download_url\":\"https://nested/wrong\"},"
        "\"browser_download_url\":\"https://x/fw.bin\"}"
        "]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://x/fw.bin", url);
}

/* Duplicate matching "browser_download_url" key inside the same matched
 * asset object, with "assets" appearing BEFORE "tag_name" so url_found
 * becomes true before tag_found (no early-stop yet) -- the second
 * occurrence exercises the !url_found false sub-branch of is_url. */
void test_bb_release_manifest_parse_github_duplicate_url_key_second_ignored(void)
{
    const char *json =
        "{\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://x/first.bin\","
        "\"browser_download_url\":\"https://x/second.bin\"}"
        "],\"tag_name\":\"v1.0.0\"}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://x/first.bin", url);
}

/* "assets" (with the matching asset) appears BEFORE "tag_name" -- so
 * url_found becomes true first, and early-stop fires when tag_name's
 * value completes (the "if (s->url_found) return
 * BB_RELEASE_MANIFEST_SINK_DONE" arm inside the is_tag branch, as
 * opposed to the is_url branch's equivalent check exercised by every
 * other test in this file where tag_name precedes assets). */
void test_bb_release_manifest_parse_github_assets_before_tag_name_early_stop(void)
{
    const char *json =
        "{\"assets\":["
        "{\"name\":\"fw.bin\",\"browser_download_url\":\"https://x/fw.bin\"}"
        "],\"tag_name\":\"v1.0.0\"}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://x/fw.bin", url);
}

/* ---------------------------------------------------------------------
 * Review findings (json-github PR): partial-write leak, invalid "assets"
 * shape, and a root-array document proving sink_begin_arr()/
 * sink_value_str_chunk()'s depth==1/key==NULL branches are reachable.
 * --------------------------------------------------------------------- */

/* A body truncated mid-"browser_download_url" (e.g. a dropped HTTP
 * connection) must not leave a PARTIAL url in url_out, AND must hard-fail
 * (not fold to BB_OK) even though tag_name was fully extracted before the
 * cut -- same "genuine scan error" path as
 * test_..._malformed_after_tag_found. Before
 * bb_release_manifest_sink_finalize()'s partial-write fix, url_out held
 * "https://x/fw" here; before its error-propagation fix, this returned
 * BB_OK, which would have let bb_ota_check_common.c's dl_url[0]=='\0'
 * no-asset guard fire and mask a dropped/corrupt fetch as "no firmware
 * asset published for this board" (no alert). */
void test_bb_release_manifest_parse_github_truncated_mid_url_leaves_no_partial_url(void)
{
    const char *full =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"fw.bin\",\"browser_download_url\":\"https://x/fw.bin\"}"
        "]}";
    const char *cut_marker = "https://x/fw";
    const char *cut_at = strstr(full, cut_marker) + strlen(cut_marker);
    size_t truncated_len = (size_t)(cut_at - full);

    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(full, truncated_len, "fw",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("", url);
}

/* A top-level STRING field, with a key length OTHER than 8, appearing
 * BEFORE "tag_name" is found -- exercises the key_len==8 FALSE sub-branch
 * of is_tag in sink_value_str_chunk(). Every other top-level-string test
 * in this file either has tag_found already true by the time a
 * differently-sized key arrives (short-circuiting is_tag's first
 * condition before key_len is ever compared) or uses an 8-char key
 * ("html_url"). */
void test_bb_release_manifest_parse_github_toplevel_short_string_key_before_tag_found(void)
{
    const char *json = "{\"id\":\"abc\",\"tag_name\":\"v1.0.0\",\"assets\":[]}";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "x",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("", url);
}

/* A top-level JSON ARRAY (not an object) -- proves sink_begin_arr()'s and
 * sink_value_str_chunk()'s depth==1/key==NULL sub-checks are REACHABLE
 * (not "structurally impossible" as the now-corrected exclusion comments
 * previously claimed -- see bb_release_manifest_json_sink.c). Harmless:
 * nothing at the root matches "tag_name"/"assets", so the result is just
 * BB_ERR_NOT_FOUND, same as any other document missing both fields. */
void test_bb_release_manifest_parse_github_root_array_document(void)
{
    const char *json = "[[1,2],\"v1.0.0\"]";
    char tag[32], url[256];
    bb_err_t ret = bb_release_manifest_parse_github(json, strlen(json), "x",
                                                    tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, ret);
}

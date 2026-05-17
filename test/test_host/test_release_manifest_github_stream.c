// Tests for the streaming GitHub release manifest parser.
//
// Each test exercises a different chunk granularity to verify that the
// state machine correctly handles key/value strings split across chunk
// boundaries.

#include "unity.h"
#include "bb_release_manifest.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Feed a payload in chunks of `chunk_size` bytes. Returns the final error
// from _feed (not _end).
static bb_err_t feed_in_chunks(bb_release_manifest_stream_ctx_t *ctx,
                                const char *payload, size_t len,
                                size_t chunk_size)
{
    size_t offset = 0;
    bb_err_t err = BB_OK;
    while (offset < len && err == BB_OK) {
        size_t n = len - offset;
        if (n > chunk_size) n = chunk_size;
        err = bb_release_manifest_parse_github_stream_feed(ctx, payload + offset, n);
        offset += n;
    }
    return err;
}

// Full round-trip helper: begin -> feed -> end, returns _end result.
static bb_err_t stream_parse(const char *json, const char *board,
                              char *tag, size_t tag_cap,
                              char *url, size_t url_cap,
                              size_t chunk_size)
{
    bb_release_manifest_stream_ctx_t ctx;
    bb_err_t err = bb_release_manifest_parse_github_stream_begin(
        &ctx, board, tag, tag_cap, url, url_cap);
    if (err != BB_OK) return err;

    feed_in_chunks(&ctx, json, strlen(json), chunk_size);
    return bb_release_manifest_parse_github_stream_end(&ctx);
}

// Canonical test payload used for chunk-granularity tests.
static const char *PAYLOAD_VALID =
    "{\"tag_name\":\"v1.2.3\","
    "\"assets\":["
    "{\"name\":\"firmware.bin\","
    "\"browser_download_url\":\"https://example.com/firmware.bin\"},"
    "{\"name\":\"other.bin\","
    "\"browser_download_url\":\"https://example.com/other.bin\"}"
    "]}";

// ---------------------------------------------------------------------------
// Argument validation
// ---------------------------------------------------------------------------

void test_stream_begin_null_ctx_returns_invalid_arg(void)
{
    char tag[32], url[256];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_release_manifest_parse_github_stream_begin(NULL, "fw", tag, sizeof(tag), url, sizeof(url)));
}

void test_stream_begin_null_board_returns_invalid_arg(void)
{
    bb_release_manifest_stream_ctx_t ctx;
    char tag[32], url[256];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_release_manifest_parse_github_stream_begin(&ctx, NULL, tag, sizeof(tag), url, sizeof(url)));
}

void test_stream_begin_null_tag_returns_invalid_arg(void)
{
    bb_release_manifest_stream_ctx_t ctx;
    char url[256];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_release_manifest_parse_github_stream_begin(&ctx, "fw", NULL, 32, url, sizeof(url)));
}

void test_stream_begin_null_url_returns_invalid_arg(void)
{
    bb_release_manifest_stream_ctx_t ctx;
    char tag[32];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_release_manifest_parse_github_stream_begin(&ctx, "fw", tag, sizeof(tag), NULL, 256));
}

void test_stream_begin_zero_tag_cap_returns_invalid_arg(void)
{
    bb_release_manifest_stream_ctx_t ctx;
    char tag[32], url[256];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_release_manifest_parse_github_stream_begin(&ctx, "fw", tag, 0, url, sizeof(url)));
}

void test_stream_begin_zero_url_cap_returns_invalid_arg(void)
{
    bb_release_manifest_stream_ctx_t ctx;
    char tag[32], url[256];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_release_manifest_parse_github_stream_begin(&ctx, "fw", tag, sizeof(tag), url, 0));
}

void test_stream_begin_cap_of_one_returns_invalid_arg(void)
{
    bb_release_manifest_stream_ctx_t ctx;
    char tag[32], url[256];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_release_manifest_parse_github_stream_begin(&ctx, "fw", tag, 1, url, sizeof(url)));
}

void test_stream_end_null_ctx_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
        bb_release_manifest_parse_github_stream_end(NULL));
}

// ---------------------------------------------------------------------------
// Basic success cases at different chunk granularities
// ---------------------------------------------------------------------------

void test_stream_whole_body_chunk(void)
{
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(PAYLOAD_VALID, "firmware",
                                tag, sizeof(tag), url, sizeof(url),
                                strlen(PAYLOAD_VALID));  // one chunk = whole body
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.2.3", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/firmware.bin", url);
}

void test_stream_256_byte_chunks(void)
{
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(PAYLOAD_VALID, "firmware",
                                tag, sizeof(tag), url, sizeof(url), 256);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.2.3", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/firmware.bin", url);
}

void test_stream_7_byte_chunks(void)
{
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(PAYLOAD_VALID, "firmware",
                                tag, sizeof(tag), url, sizeof(url), 7);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.2.3", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/firmware.bin", url);
}

void test_stream_1_byte_chunks(void)
{
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(PAYLOAD_VALID, "firmware",
                                tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.2.3", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/firmware.bin", url);
}

// ---------------------------------------------------------------------------
// Multiple assets — first non-matching, second matching
// ---------------------------------------------------------------------------

void test_stream_skips_non_matching_first_asset(void)
{
    const char *json =
        "{\"tag_name\":\"v2.0.0\",\"assets\":["
        "{\"name\":\"other.bin\",\"browser_download_url\":\"https://x/other.bin\"},"
        "{\"name\":\"target.bin\",\"browser_download_url\":\"https://x/target.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "target", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v2.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://x/target.bin", url);
}

void test_stream_skips_non_matching_first_asset_256(void)
{
    const char *json =
        "{\"tag_name\":\"v2.0.0\",\"assets\":["
        "{\"name\":\"other.bin\",\"browser_download_url\":\"https://x/other.bin\"},"
        "{\"name\":\"target.bin\",\"browser_download_url\":\"https://x/target.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "target", tag, sizeof(tag), url, sizeof(url), 256);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("https://x/target.bin", url);
}

// ---------------------------------------------------------------------------
// Missing required fields
// ---------------------------------------------------------------------------

void test_stream_missing_tag_returns_not_found(void)
{
    const char *json = "{\"assets\":[{\"name\":\"fw.bin\",\"browser_download_url\":\"https://x/fw.bin\"}]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, ret);
}

void test_stream_missing_assets_returns_not_found(void)
{
    const char *json = "{\"tag_name\":\"v1.0.0\"}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 7);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, ret);
}

void test_stream_no_matching_asset_returns_not_found(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"other.bin\",\"browser_download_url\":\"https://x/other.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 256);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, ret);
}

void test_stream_empty_assets_array_returns_not_found(void)
{
    const char *json = "{\"tag_name\":\"v1.0.0\",\"assets\":[]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 7);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, ret);
}

void test_stream_bad_json_returns_not_found(void)
{
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse("not json", "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, ret);
}

// ---------------------------------------------------------------------------
// Asset with no browser_download_url
// ---------------------------------------------------------------------------

void test_stream_asset_missing_url_returns_not_found(void)
{
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"fw.bin\",\"size\":42}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 7);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, ret);
}

// ---------------------------------------------------------------------------
// TaipanMiner-style realistic payload (board name with hyphens)
// ---------------------------------------------------------------------------

void test_stream_taipanminer_board_name(void)
{
    const char *json =
        "{\"tag_name\":\"v0.4.1\","
        "\"assets\":["
        "{\"name\":\"taipanminer-bitaxe-601.bin\","
        "\"browser_download_url\":\"https://github.com/dangernoodle-io/TaipanMiner/releases/download/v0.4.1/taipanminer-bitaxe-601.bin\"},"
        "{\"name\":\"taipanminer-bitaxe-650.bin\","
        "\"browser_download_url\":\"https://github.com/dangernoodle-io/TaipanMiner/releases/download/v0.4.1/taipanminer-bitaxe-650.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "taipanminer-bitaxe-650",
                                tag, sizeof(tag), url, sizeof(url), 7);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v0.4.1", tag);
    TEST_ASSERT_EQUAL_STRING(
        "https://github.com/dangernoodle-io/TaipanMiner/releases/download/v0.4.1/taipanminer-bitaxe-650.bin",
        url);
}

// ---------------------------------------------------------------------------
// Backslash escape in URL (e.g. \/ in a value)
// ---------------------------------------------------------------------------

void test_stream_backslash_slash_in_url(void)
{
    // GitHub sometimes emits \/ in JSON strings
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https:\\/\\/example.com\\/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    // Feed 1 byte at a time to stress escape split
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

// ---------------------------------------------------------------------------
// feed after error is a no-op
// ---------------------------------------------------------------------------

void test_stream_feed_after_error_is_noop(void)
{
    bb_release_manifest_stream_ctx_t ctx;
    char tag[32] = {0}, url[256] = {0};
    bb_release_manifest_parse_github_stream_begin(
        &ctx, "fw", tag, sizeof(tag), url, sizeof(url));

    // Feed valid data to find tag_name, then stop before finding assets.
    // Force an early end -> not found.
    const char *partial = "{\"tag_name\":\"v1.0.0\"";
    bb_release_manifest_parse_github_stream_feed(&ctx, partial, strlen(partial));
    bb_err_t end1 = bb_release_manifest_parse_github_stream_end(&ctx);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, end1);

    // More feeds should not crash or change the result.
    bb_release_manifest_parse_github_stream_feed(&ctx, "}", 1);
}

// ---------------------------------------------------------------------------
// Whitespace in JSON
// ---------------------------------------------------------------------------

void test_stream_whitespace_around_colons_1byte(void)
{
    const char *json =
        "{ \"tag_name\" : \"v1.0.0\" , \"assets\" : ["
        "{ \"name\" : \"x.bin\" , \"browser_download_url\" : \"https://example.com/x.bin\" }"
        "] }";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "x", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/x.bin", url);
}

// ---------------------------------------------------------------------------
// Backslash-backslash in URL (decoded '\' → '\')
// ---------------------------------------------------------------------------

void test_stream_backslash_backslash_in_url(void)
{
    // A URL containing \\path should decode to \path.
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://example.com\\\\fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com\\fw.bin", url);
}

// ---------------------------------------------------------------------------
// \u escape in URL is dropped (character skipped)
// ---------------------------------------------------------------------------

void test_stream_unicode_escape_in_url_dropped(void)
{
    // A (letter 'A') is silently dropped by the parser.
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://example.com/\\u0041fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    // A is dropped; remaining 4 hex digits are consumed as normal chars
    // and appended. The exact result is implementation-defined (drop 'u',
    // keep hex digits) — just verify parse succeeds.
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
}

// ---------------------------------------------------------------------------
// Backslash escape inside asset name (CHECK_NAME path)
// ---------------------------------------------------------------------------

void test_stream_escape_in_asset_name_no_match(void)
{
    // Asset name with a \\ inside it. The decoded name won't match "fw" so
    // we get NOT_FOUND, but the escape path in copy_tmp_char is exercised.
    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"f\\\\w.bin\","
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    // "f\w.bin" != "fw.bin" so no URL match -> NOT_FOUND
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, ret);
}

// ---------------------------------------------------------------------------
// Board name truncation (> BOARD_NAME_MAX - 4 - 1 chars)
// ---------------------------------------------------------------------------

void test_stream_board_name_truncation(void)
{
    // Pass a 200-char board name — exceeds the 128-byte budget so it's
    // truncated internally. Parse still succeeds structurally (returns
    // NOT_FOUND because the asset name won't match the truncated form).
    char long_board[201];
    memset(long_board, 'a', 200);
    long_board[200] = '\0';

    const char *json =
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_release_manifest_stream_ctx_t ctx;
    bb_err_t err = bb_release_manifest_parse_github_stream_begin(
        &ctx, long_board, tag, sizeof(tag), url, sizeof(url));
    TEST_ASSERT_EQUAL(BB_OK, err);

    feed_in_chunks(&ctx, json, strlen(json), 256);
    // NOT_FOUND: truncated board name doesn't match "fw"
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
        bb_release_manifest_parse_github_stream_end(&ctx));
}

// ---------------------------------------------------------------------------
// PH_SKIP_STRING with backslash escape
// ---------------------------------------------------------------------------

void test_stream_skip_string_with_escape(void)
{
    // A top-level string value that is skipped contains a backslash escape.
    // This exercises PH_SKIP_STRING in_escape and closing '"' paths.
    const char *json =
        "{\"tag_name\":\"v1.0.0\","
        "\"description\":\"some\\\\escaped value\","
        "\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

// ---------------------------------------------------------------------------
// PH_SCAN_KEY bracket tracking — nested array at top level
// ---------------------------------------------------------------------------

void test_stream_toplevel_nested_array_skipped(void)
{
    // A top-level key whose value is an array (not "assets") exercises the
    // skip-array path (PH_AFTER_COLON '[' when after_key != ENTER_ASSETS)
    // and also the PH_SCAN_KEY '['/']' depth tracking.
    const char *json =
        "{\"tag_name\":\"v1.0.0\","
        "\"other\":[1,2,3],"
        "\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

// ---------------------------------------------------------------------------
// PH_AFTER_COLON nested object skip path
// ---------------------------------------------------------------------------

void test_stream_toplevel_nested_object_skipped(void)
{
    // A top-level key whose value is a nested object exercises the
    // PH_AFTER_COLON '{' skip-depth path.
    const char *json =
        "{\"tag_name\":\"v1.0.0\","
        "\"meta\":{\"build\":\"abc\",\"rev\":42},"
        "\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

// ---------------------------------------------------------------------------
// PH_SKIP_DEPTH with strings containing braces
// ---------------------------------------------------------------------------

void test_stream_skip_depth_string_with_braces(void)
{
    // A skipped nested object contains a string value with '{' and '}' chars.
    // The parser must not count those as depth changes.
    const char *json =
        "{\"tag_name\":\"v1.0.0\","
        "\"meta\":{\"fmt\":\"{hello}\",\"rev\":1},"
        "\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

// ---------------------------------------------------------------------------
// PH_SKIP_DEPTH with backslash escape inside string (in-string mode)
// ---------------------------------------------------------------------------

void test_stream_skip_depth_string_with_escape(void)
{
    // A skipped nested object has a string value containing a backslash escape.
    // Exercises the in_escape path inside PH_SKIP_DEPTH string mode.
    const char *json =
        "{\"tag_name\":\"v1.0.0\","
        "\"meta\":{\"path\":\"C:\\\\Windows\\\\\"},"
        "\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

// ---------------------------------------------------------------------------
// PH_IN_KEY escape handling
// ---------------------------------------------------------------------------

void test_stream_escaped_key_char_ignored(void)
{
    // A JSON key with a backslash escape. The escaped char is accumulated
    // into key_buf but won't match any known key, so it's ignored.
    // Exercises PH_IN_KEY key_in_escape path.
    const char *json =
        "{\"tag_name\":\"v1.0.0\","
        "\"as\\\\sets\":\"ignored\","
        "\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

// ---------------------------------------------------------------------------
// PH_SCAN_KEY: assets array exit via ']' depth tracking
// ---------------------------------------------------------------------------

void test_stream_assets_array_exit_via_scan_key(void)
{
    // Verify that after assets[], additional top-level keys are ignored and
    // the parser exits cleanly. This exercises PH_SCAN_KEY ']' depth tracking.
    const char *json =
        "{\"tag_name\":\"v1.0.0\","
        "\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "],\"extra\":\"trailing\"}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

// ---------------------------------------------------------------------------
// Asset with scalar (numeric) field exercises PH_AFTER_COLON scalar path
// ---------------------------------------------------------------------------

void test_stream_asset_scalar_field_skipped(void)
{
    // An asset with a numeric "size" field before "browser_download_url"
    // exercises the PH_AFTER_COLON scalar-skip path inside an asset object.
    const char *json =
        "{\"tag_name\":\"v1.0.0\","
        "\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"size\":12345,"
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

// ---------------------------------------------------------------------------
// Top-level scalar field — exercises in_asset_obj=false side of the ternary
// in PH_AFTER_COLON scalar-skip path (GitHub's real payload has top-level
// scalars like "id":42, "draft":false before "assets").
// ---------------------------------------------------------------------------

void test_stream_toplevel_scalar_field_skipped(void)
{
    const char *json =
        "{\"id\":42,"
        "\"draft\":false,"
        "\"tag_name\":\"v1.0.0\","
        "\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

// ---------------------------------------------------------------------------
// Asset with nested array field — exercises in_asset_obj=true side of the
// ternary in PH_AFTER_COLON array-skip path (gcc reports as uncovered branch
// when only the top-level array-skip case is tested).
// ---------------------------------------------------------------------------

void test_stream_asset_with_nested_array_field_skipped(void)
{
    // Asset has a "reactions"-style array property that the parser must skip,
    // returning to PH_IN_ASSET_OBJECT (not PH_SCAN_KEY).
    const char *json =
        "{\"tag_name\":\"v1.0.0\","
        "\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"reactions\":[1,2,3],"
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

// ---------------------------------------------------------------------------
// Asset with nested object field — exercises in_asset_obj=true side of the
// ternary in PH_AFTER_COLON object-skip path.
// ---------------------------------------------------------------------------

void test_stream_asset_with_nested_object_field_skipped(void)
{
    // Asset has an "uploader"-style nested object that the parser must skip,
    // returning to PH_IN_ASSET_OBJECT.
    const char *json =
        "{\"tag_name\":\"v1.0.0\","
        "\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"uploader\":{\"login\":\"bot\",\"id\":1},"
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

// ---------------------------------------------------------------------------
// copy_tmp_char truncation (asset name >= BOARD_NAME_MAX chars)
// ---------------------------------------------------------------------------

void test_stream_asset_name_at_capacity_truncated_no_match(void)
{
    // Asset name is 132 chars — fills copy_tmp exactly, exercising the
    // copy_tmp_char false branch (silently truncate overflow).
    // The truncated name won't equal "fw.bin" so the result is NOT_FOUND.
    char long_name[133 + 7];  // 132 chars + ".bin" extra + quotes + null
    memset(long_name, 'x', 132);
    long_name[132] = '\0';

    // Build: {"tag_name":"v1.0.0","assets":[{"name":"<132x>","browser_download_url":"https://example.com/fw.bin"}]}
    char json[512];
    snprintf(json, sizeof(json),
        "{\"tag_name\":\"v1.0.0\",\"assets\":["
        "{\"name\":\"%s\","
        "\"browser_download_url\":\"https://example.com/fw.bin\"}]}",
        long_name);

    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, ret);
}

// ---------------------------------------------------------------------------
// PH_IN_KEY: key length at capacity (>= KEY_MAX - 1 = 23)
// ---------------------------------------------------------------------------

void test_stream_very_long_key_truncated_ignored(void)
{
    // A JSON key of 26 chars (> KEY_MAX-1=23) exercises the false branch of
    // `if (s->key_len < KEY_MAX - 1)` in PH_IN_KEY for regular chars (L365).
    // The key won't match any known key so parsing proceeds normally.
    const char *json =
        "{\"tag_name\":\"v1.0.0\","
        "\"abcdefghijklmnopqrstuvwxyz\":\"ignored\","
        "\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

void test_stream_long_key_with_escape_at_capacity_ignored(void)
{
    // A key of 24 normal chars followed by a backslash-escape exercises the
    // false branch of `if (s->key_len < KEY_MAX - 1)` in the key_in_escape
    // path (L355).  After 23 chars the buffer is full; the escaped char is
    // silently dropped.  The key (truncated) won't match any known key.
    const char *json =
        "{\"tag_name\":\"v1.0.0\","
        "\"aaaaaaaaaaaaaaaaaaaaaaaa\\\\z\":\"ignored\","
        "\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

// ---------------------------------------------------------------------------
// PH_SKIP_DEPTH: '[' inside skipped block (exercises c=='[' side of ||)
// ---------------------------------------------------------------------------

void test_stream_skip_depth_nested_array_inside_object(void)
{
    // A skipped nested object contains an array value, which causes '[' to be
    // processed when skip_depth > 0.  This exercises the `c == '['` branch of
    // `if (c == '{' || c == '[') s->skip_depth++` in PH_SKIP_DEPTH.
    const char *json =
        "{\"tag_name\":\"v1.0.0\","
        "\"meta\":{\"tags\":[\"a\",\"b\"]},"
        "\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

// ---------------------------------------------------------------------------
// PH_SKIP_DEPTH: depth > 1 (skip_depth stays > 0 after first '}'/'[')
// ---------------------------------------------------------------------------

void test_stream_skip_depth_multi_level_nesting(void)
{
    // A skipped nested object contains a doubly-nested object, which means
    // skip_depth reaches 3 and the `if (s->skip_depth == 0)` guard fires
    // false (depth not yet zero) when the inner '}' is seen.
    const char *json =
        "{\"tag_name\":\"v1.0.0\","
        "\"meta\":{\"inner\":{\"key\":1}},"
        "\"assets\":["
        "{\"name\":\"fw.bin\","
        "\"browser_download_url\":\"https://example.com/fw.bin\"}"
        "]}";
    char tag[32] = {0}, url[256] = {0};
    bb_err_t ret = stream_parse(json, "fw", tag, sizeof(tag), url, sizeof(url), 1);
    TEST_ASSERT_EQUAL(BB_OK, ret);
    TEST_ASSERT_EQUAL_STRING("v1.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", url);
}

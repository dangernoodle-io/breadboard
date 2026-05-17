// Tests for the streaming manifest-fetch path in bb_ota_pull.
//
// Exercises ota_fetch_manifest via the BB_OTA_PULL_TESTING hook
// bb_ota_pull_fetch_manifest_for_test. The host mock delivers the JSON
// body in ~256-byte chunks via bb_http_client_get_stream, matching the
// realistic on-device delivery pattern.

#include "unity.h"
#include "bb_ota_pull.h"
#include "bb_ota_pull_test_hooks.h"
#include "bb_http_client_host.h"
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// Minimal valid GitHub releases/latest payload. "firmware" asset matches
// the board name set in each test via bb_ota_pull_set_firmware_board().
static const char *MANIFEST_VALID =
    "{\"tag_name\":\"v1.2.3\","
    "\"assets\":["
    "{\"name\":\"firmware.bin\","
    "\"browser_download_url\":\"https://example.com/firmware.bin\"}"
    "]}";

// Payload where asset name does not match any board.
static const char *MANIFEST_NO_MATCH =
    "{\"tag_name\":\"v1.2.3\","
    "\"assets\":["
    "{\"name\":\"other-board.bin\","
    "\"browser_download_url\":\"https://example.com/other-board.bin\"}"
    "]}";

// GitHub-realistic payload: top-level scalars ("id", "draft") before
// "tag_name", asset with nested "uploader" object and "reactions" array.
// Exercises gcc-only trailing branches in the stream parser.
static const char *MANIFEST_REALISTIC =
    "{\"id\":123456,"
    "\"draft\":false,"
    "\"prerelease\":false,"
    "\"tag_name\":\"v2.0.0\","
    "\"assets\":["
    "{\"name\":\"myboard.bin\","
    "\"size\":512000,"
    "\"uploader\":{\"login\":\"bot\",\"id\":1},"
    "\"reactions\":[1,2,3],"
    "\"browser_download_url\":\"https://example.com/myboard.bin\"}"
    "]}";

static void reset_world(void)
{
    bb_http_client_clear_mock();
    bb_ota_pull_set_releases_url("http://example.com/releases/latest");
    bb_ota_pull_set_firmware_board("firmware");
}

// ---------------------------------------------------------------------------
// Manifest fetch succeeds — version extracted correctly
// ---------------------------------------------------------------------------

void test_ota_pull_manifest_fetch_success(void)
{
    reset_world();
    bb_http_client_set_mock_response(MANIFEST_VALID, strlen(MANIFEST_VALID), 200);

    char tag[32] = {0};
    char url[256] = {0};
    bb_err_t err = bb_ota_pull_fetch_manifest_for_test(tag, sizeof(tag),
                                                       url, sizeof(url));
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("v1.2.3", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/firmware.bin", url);
}

// ---------------------------------------------------------------------------
// Manifest fetch fails — transport error
// ---------------------------------------------------------------------------

void test_ota_pull_manifest_transport_failure(void)
{
    reset_world();
    bb_http_client_set_mock_transport_error(BB_ERR_INVALID_STATE);

    char tag[32] = {0};
    char url[256] = {0};
    bb_err_t err = bb_ota_pull_fetch_manifest_for_test(tag, sizeof(tag),
                                                       url, sizeof(url));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
}

// ---------------------------------------------------------------------------
// Manifest fetch fails — non-200 HTTP status
// ---------------------------------------------------------------------------

void test_ota_pull_manifest_http_404(void)
{
    reset_world();
    bb_http_client_set_mock_response("Not Found", 9, 404);

    char tag[32] = {0};
    char url[256] = {0};
    bb_err_t err = bb_ota_pull_fetch_manifest_for_test(tag, sizeof(tag),
                                                       url, sizeof(url));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err);
}

// ---------------------------------------------------------------------------
// Manifest parses but board asset not found
// ---------------------------------------------------------------------------

void test_ota_pull_manifest_board_asset_not_found(void)
{
    reset_world();
    // "firmware" board set in reset_world; fixture has "other-board" only.
    bb_http_client_set_mock_response(MANIFEST_NO_MATCH, strlen(MANIFEST_NO_MATCH), 200);

    char tag[32] = {0};
    char url[256] = {0};
    bb_err_t err = bb_ota_pull_fetch_manifest_for_test(tag, sizeof(tag),
                                                       url, sizeof(url));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
}

// ---------------------------------------------------------------------------
// Manifest body is not valid JSON
// ---------------------------------------------------------------------------

void test_ota_pull_manifest_bad_json(void)
{
    reset_world();
    bb_http_client_set_mock_response("not json", 8, 200);

    char tag[32] = {0};
    char url[256] = {0};
    bb_err_t err = bb_ota_pull_fetch_manifest_for_test(tag, sizeof(tag),
                                                       url, sizeof(url));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
}

// ---------------------------------------------------------------------------
// Realistic GitHub payload: top-level scalars + nested asset fields
// ---------------------------------------------------------------------------

void test_ota_pull_manifest_realistic_github_payload(void)
{
    reset_world();
    bb_ota_pull_set_firmware_board("myboard");
    bb_http_client_set_mock_response(MANIFEST_REALISTIC, strlen(MANIFEST_REALISTIC), 200);

    char tag[32] = {0};
    char url[256] = {0};
    bb_err_t err = bb_ota_pull_fetch_manifest_for_test(tag, sizeof(tag),
                                                       url, sizeof(url));
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("v2.0.0", tag);
    TEST_ASSERT_EQUAL_STRING("https://example.com/myboard.bin", url);
}

// ---------------------------------------------------------------------------
// Board name fallback ("unknown") when no board is set
// ---------------------------------------------------------------------------

void test_ota_pull_manifest_fallback_board_name(void)
{
    // Clear the board name — ota_fetch_manifest uses "unknown" as fallback.
    // Payload has no "unknown.bin" asset so parse returns NOT_FOUND.
    bb_http_client_clear_mock();
    bb_ota_pull_set_releases_url("http://example.com/releases/latest");
    bb_ota_pull_set_firmware_board(NULL);

    bb_http_client_set_mock_response(MANIFEST_VALID, strlen(MANIFEST_VALID), 200);

    char tag[32] = {0};
    char url[256] = {0};
    bb_err_t err = bb_ota_pull_fetch_manifest_for_test(tag, sizeof(tag),
                                                       url, sizeof(url));
    // "firmware.bin" does not match "unknown.bin" → NOT_FOUND
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, err);
}

// ---------------------------------------------------------------------------
// OTA pull proceeds when manifest is found and asset matches
// (version comparison is ESP-IDF-only; on host we verify tag/url are correct)
// ---------------------------------------------------------------------------

void test_ota_pull_manifest_asset_found_ota_proceeds(void)
{
    reset_world();
    bb_ota_pull_set_firmware_board("firmware");
    bb_http_client_set_mock_response(MANIFEST_VALID, strlen(MANIFEST_VALID), 200);

    char tag[32] = {0};
    char url[256] = {0};
    bb_err_t err = bb_ota_pull_fetch_manifest_for_test(tag, sizeof(tag),
                                                       url, sizeof(url));
    TEST_ASSERT_EQUAL(BB_OK, err);
    // Verify the fields that ota_pull_check would use to trigger the OTA worker.
    TEST_ASSERT_EQUAL_STRING("v1.2.3", tag);
    TEST_ASSERT_GREATER_THAN(0, (int)strlen(url));
}

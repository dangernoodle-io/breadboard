// Host tests for bb_wifi_http_scan_wire (POST /api/wifi/scan emit,
// object-wrapped {"aps":[...]} shape). Renders the top-level
// bb_wifi_http_scan_wire_desc via bb_serialize_json_render() (the same
// one-shot entry point every other object-shaped route in this component
// ultimately drives through bb_http_serialize_stream()) and asserts the
// resulting JSON string exactly, byte for byte.

#include "unity.h"

#include "../../components/bb_wifi_http/bb_wifi_http_scan_wire_priv.h"

#include "bb_serialize_json.h"

#include <stdio.h>
#include <string.h>

#define RENDER_BUF_BYTES 4096

// Renders `aps` (via the same copy_rows() helper the production fill fn
// uses) as the top-level {"aps":[...]} object and returns the NUL-terminated
// JSON string in `out_buf` (caller-owned, RENDER_BUF_BYTES capacity).
static void scan_render(const bb_wifi_ap_t *aps, size_t count, char *out_buf)
{
    bb_wifi_http_scan_wire_t snap;
    memset(&snap, 0, sizeof(snap));
    bb_wifi_http_scan_wire_copy_rows(snap.aps_items, aps, count);
    snap.aps.items = snap.aps_items;
    snap.aps.count = count;

    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&bb_wifi_http_scan_wire_desc, &snap,
                                            out_buf, RENDER_BUF_BYTES, &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);
}

// ---------------------------------------------------------------------------
// 1. Zero APs -> exactly {"aps":[]}.
// ---------------------------------------------------------------------------

void test_wifi_scan_wire_expected_json_zero_aps(void)
{
    char buf[RENDER_BUF_BYTES];
    scan_render(NULL, 0, buf);
    TEST_ASSERT_EQUAL_STRING("{\"aps\":[]}", buf);
}

// ---------------------------------------------------------------------------
// 2. One AP.
// ---------------------------------------------------------------------------

void test_wifi_scan_wire_expected_json_one_ap(void)
{
    bb_wifi_ap_t aps[1] = {
        { .ssid = "Solo", .rssi = -60, .secure = true },
    };

    char buf[RENDER_BUF_BYTES];
    scan_render(aps, 1, buf);
    TEST_ASSERT_EQUAL_STRING("{\"aps\":[{\"ssid\":\"Solo\",\"rssi\":-60,\"secure\":true}]}", buf);
}

// ---------------------------------------------------------------------------
// 3. WIFI_SCAN_MAX APs (identical rows -- the max-cap shape).
// ---------------------------------------------------------------------------

void test_wifi_scan_wire_expected_json_max_cap(void)
{
    bb_wifi_ap_t aps[WIFI_SCAN_MAX];
    for (int i = 0; i < WIFI_SCAN_MAX; i++) {
        strncpy(aps[i].ssid, "AP", sizeof(aps[i].ssid) - 1);
        aps[i].ssid[sizeof(aps[i].ssid) - 1] = '\0';
        aps[i].rssi = -40;
        aps[i].secure = false;
    }

    char buf[RENDER_BUF_BYTES];
    scan_render(aps, WIFI_SCAN_MAX, buf);

    char expected[RENDER_BUF_BYTES];
    size_t off = 0;
    off += (size_t)snprintf(expected + off, sizeof(expected) - off, "{\"aps\":[");
    for (int i = 0; i < WIFI_SCAN_MAX; i++) {
        if (i > 0) expected[off++] = ',';
        off += (size_t)snprintf(expected + off, sizeof(expected) - off,
                                 "{\"ssid\":\"AP\",\"rssi\":-40,\"secure\":false}");
    }
    off += (size_t)snprintf(expected + off, sizeof(expected) - off, "]}");

    TEST_ASSERT_EQUAL_STRING(expected, buf);
}

// ---------------------------------------------------------------------------
// 4. SSID with JSON-special characters (quote + backslash) -- exercises
// escaping inside the wrapped object.
// ---------------------------------------------------------------------------

void test_wifi_scan_wire_expected_json_escaped_ssid(void)
{
    bb_wifi_ap_t aps[1] = {
        { .ssid = "Weird\"Net\\Name", .rssi = -55, .secure = false },
    };

    char buf[RENDER_BUF_BYTES];
    scan_render(aps, 1, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"aps\":[{\"ssid\":\"Weird\\\"Net\\\\Name\",\"rssi\":-55,\"secure\":false}]}", buf);
}

// ---------------------------------------------------------------------------
// 5. SSID with a control character (0x01-0x1F) -- exercises the \u00XX
// escape path inside the wrapped object. (Not 0x00 -- that NUL-terminates
// the C string.)
// ---------------------------------------------------------------------------

void test_wifi_scan_wire_expected_json_control_char_ssid(void)
{
    bb_wifi_ap_t aps[1] = {
        // Adjacent-literal split ("\x01" "Char") is load-bearing: C's \xHH
        // hex escape greedily consumes every following hex-digit character
        // (including 'C'), so an unsplit "Ctrl\x01Char" would parse as
        // \x01C (0x1C) followed by "har" -- not the intended 0x01 + "Char".
        { .ssid = "Ctrl\x01" "Char", .rssi = -55, .secure = false },
    };

    char buf[RENDER_BUF_BYTES];
    scan_render(aps, 1, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"aps\":[{\"ssid\":\"Ctrl\\u0001Char\",\"rssi\":-55,\"secure\":false}]}", buf);
}

// ---------------------------------------------------------------------------
// 6. bb_wifi_http_scan_wire_fill() itself -- the async-scan + cached-read +
// row-copy + carrier-wiring glue (bb_wifi_http_scan_wire_copy_rows() above is
// exercised directly by the expected-JSON tests; this covers the thin fill()
// wrapper that calls it). The host bb_wifi shim's bb_wifi_scan_get_cached()
// always returns 0 results, so this pins the count==0 fill path -- dst->aps
// must come back wired to dst->aps_items with count 0, not left
// uninitialized.
// ---------------------------------------------------------------------------

void test_wifi_scan_wire_fill_populates_snapshot(void)
{
    bb_wifi_http_scan_wire_t snap;
    memset(&snap, 0xAA, sizeof(snap));

    bb_wifi_http_scan_wire_fill(&snap);

    TEST_ASSERT_EQUAL_size_t(0, snap.aps.count);
    TEST_ASSERT_EQUAL_PTR(snap.aps_items, snap.aps.items);
}

// ---------------------------------------------------------------------------
// 7. Row field-count invariant -- guards BB_WIFI_HTTP_SCAN_AP_ROW_N_FIELDS
// (bb_wifi_http_scan_wire.c) against drift, same precedent as
// test_bb_diag_storage_partitions_row_field_count_matches_partition_serialize.
// ---------------------------------------------------------------------------

void test_wifi_scan_wire_row_field_count_matches(void)
{
    TEST_ASSERT_EQUAL_UINT16(3, bb_wifi_http_scan_ap_wire_n_fields);
}

// ---------------------------------------------------------------------------
// 8. bb_wifi_http_scan_wire_clamp_count() -- guards the int-to-size_t
// narrowing in bb_wifi_http_scan_wire_fill() against a negative or over-cap
// bb_wifi_scan_get_cached() return. All three source branches (count<=0,
// count>WIFI_SCAN_MAX, in-range passthrough) are exercised explicitly since
// the host bb_wifi shim's bb_wifi_scan_get_cached() always returns 0 --
// fill() alone can never reach the positive-count branches.
// ---------------------------------------------------------------------------

void test_wifi_scan_wire_clamp_count_negative_clamps_to_zero(void)
{
    TEST_ASSERT_EQUAL_size_t(0, bb_wifi_http_scan_wire_clamp_count(-1));
}

void test_wifi_scan_wire_clamp_count_over_cap_clamps_to_max(void)
{
    TEST_ASSERT_EQUAL_size_t((size_t)WIFI_SCAN_MAX,
                              bb_wifi_http_scan_wire_clamp_count(WIFI_SCAN_MAX + 5));
}

void test_wifi_scan_wire_clamp_count_in_range_passes_through(void)
{
    TEST_ASSERT_EQUAL_size_t(5, bb_wifi_http_scan_wire_clamp_count(5));
}

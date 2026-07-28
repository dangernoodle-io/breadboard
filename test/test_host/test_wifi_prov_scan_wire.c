// Host tests for bb_wifi_prov_scan_wire (bb_wifi_prov's own POST
// /api/wifi/scan emit, B1-809 portal). Covers the pure classifier
// (bb_wifi_prov_scan_classify_state), the state->string mapping
// (bb_wifi_prov_scan_state_str), and the wire descriptor's rendered JSON
// (via bb_serialize_json_render()) exactly, byte for byte. The ESP-IDF
// route glue itself (prov_scan_handler in platform/espidf/bb_wifi_prov/
// bb_wifi_prov.c -- query-param extraction, bb_wifi_scan_start_async()/
// bb_wifi_scan_get_cached()/bb_wifi_scan_wait_idle() side effects, route
// registration/dup-tolerance) is NOT host-testable and is validated on
// hardware instead, same posture bb_wifi_prov_start()'s own doc comment
// documents for its registration-order/error-propagation branches.

#include "unity.h"

#include "../../components/bb_wifi_prov/bb_wifi_prov_scan_wire.h"

#include "bb_serialize_json.h"

#include <stdio.h>
#include <string.h>

#define RENDER_BUF_BYTES 4096

// ---------------------------------------------------------------------------
// bb_wifi_prov_scan_classify_state() -- all 3 outcomes across all 4 input
// combinations (ever_requested x idle), since the 2 "ever_requested==false"
// rows both collapse to NOT_STARTED regardless of `idle`.
// ---------------------------------------------------------------------------

void test_wifi_prov_scan_classify_state_never_requested_not_idle_is_not_started(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_PROV_SCAN_NOT_STARTED,
                       bb_wifi_prov_scan_classify_state(false, false));
}

void test_wifi_prov_scan_classify_state_never_requested_idle_is_not_started(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_PROV_SCAN_NOT_STARTED,
                       bb_wifi_prov_scan_classify_state(false, true));
}

void test_wifi_prov_scan_classify_state_requested_not_idle_is_pending(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_PROV_SCAN_PENDING,
                       bb_wifi_prov_scan_classify_state(true, false));
}

void test_wifi_prov_scan_classify_state_requested_idle_is_ready(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_PROV_SCAN_READY,
                       bb_wifi_prov_scan_classify_state(true, true));
}

// ---------------------------------------------------------------------------
// bb_wifi_prov_scan_state_str()
// ---------------------------------------------------------------------------

void test_wifi_prov_scan_state_str_not_started(void)
{
    TEST_ASSERT_EQUAL_STRING("not_started",
                              bb_wifi_prov_scan_state_str(BB_WIFI_PROV_SCAN_NOT_STARTED));
}

void test_wifi_prov_scan_state_str_pending(void)
{
    TEST_ASSERT_EQUAL_STRING("pending",
                              bb_wifi_prov_scan_state_str(BB_WIFI_PROV_SCAN_PENDING));
}

void test_wifi_prov_scan_state_str_ready(void)
{
    TEST_ASSERT_EQUAL_STRING("ready",
                              bb_wifi_prov_scan_state_str(BB_WIFI_PROV_SCAN_READY));
}

// Out-of-range value -- defensive fallback branch (bb_wifi_prov_scan_wire.h's
// doc comment on this function).
void test_wifi_prov_scan_state_str_unknown_defaults_not_started(void)
{
    TEST_ASSERT_EQUAL_STRING("not_started",
                              bb_wifi_prov_scan_state_str((bb_wifi_prov_scan_state_t)99));
}

// ---------------------------------------------------------------------------
// bb_wifi_prov_scan_wire_clamp_count() -- same 3-branch shape as
// bb_wifi_http_scan_wire_clamp_count()'s precedent (test_wifi_scan_wire.c).
// ---------------------------------------------------------------------------

void test_wifi_prov_scan_wire_clamp_count_negative_clamps_to_zero(void)
{
    TEST_ASSERT_EQUAL_size_t(0, bb_wifi_prov_scan_wire_clamp_count(-1));
}

void test_wifi_prov_scan_wire_clamp_count_over_cap_clamps_to_max(void)
{
    TEST_ASSERT_EQUAL_size_t((size_t)WIFI_SCAN_MAX,
                              bb_wifi_prov_scan_wire_clamp_count(WIFI_SCAN_MAX + 5));
}

void test_wifi_prov_scan_wire_clamp_count_in_range_passes_through(void)
{
    TEST_ASSERT_EQUAL_size_t(5, bb_wifi_prov_scan_wire_clamp_count(5));
}

// ---------------------------------------------------------------------------
// bb_wifi_prov_scan_wire_fill() -- the pure snapshot builder (row-copy +
// carrier-wiring + state string), independent of any live scan.
// ---------------------------------------------------------------------------

void test_wifi_prov_scan_wire_fill_zero_aps_not_started(void)
{
    bb_wifi_prov_scan_wire_t snap;
    memset(&snap, 0xAA, sizeof(snap));

    bb_wifi_prov_scan_wire_fill(&snap, NULL, 0, BB_WIFI_PROV_SCAN_NOT_STARTED);

    TEST_ASSERT_EQUAL_size_t(0, snap.aps.count);
    TEST_ASSERT_EQUAL_PTR(snap.aps_items, snap.aps.items);
    TEST_ASSERT_EQUAL_STRING("not_started", snap.state);
}

void test_wifi_prov_scan_wire_fill_clamps_and_copies_rows(void)
{
    bb_wifi_ap_t aps[1] = {
        { .ssid = "Solo", .rssi = -60, .secure = true },
    };

    bb_wifi_prov_scan_wire_t snap;
    memset(&snap, 0xAA, sizeof(snap));

    bb_wifi_prov_scan_wire_fill(&snap, aps, 1, BB_WIFI_PROV_SCAN_READY);

    TEST_ASSERT_EQUAL_size_t(1, snap.aps.count);
    TEST_ASSERT_EQUAL_STRING("Solo", snap.aps_items[0].ssid);
    TEST_ASSERT_EQUAL_INT64(-60, snap.aps_items[0].rssi);
    TEST_ASSERT_TRUE(snap.aps_items[0].secure);
    TEST_ASSERT_EQUAL_STRING("ready", snap.state);
}

// ---------------------------------------------------------------------------
// Row field-count invariant -- guards BB_WIFI_PROV_SCAN_AP_ROW_N_FIELDS
// (bb_wifi_prov_scan_wire.c) against drift, same precedent as
// test_wifi_scan_wire_row_field_count_matches.
// ---------------------------------------------------------------------------

void test_wifi_prov_scan_wire_row_field_count_matches(void)
{
    TEST_ASSERT_EQUAL_UINT16(3, bb_wifi_prov_scan_ap_wire_n_fields);
}

// ---------------------------------------------------------------------------
// Rendered JSON -- asserts the {"aps":[...],"state":...} shape exactly.
// ---------------------------------------------------------------------------

void test_wifi_prov_scan_wire_expected_json_zero_aps_not_started(void)
{
    bb_wifi_prov_scan_wire_t snap;
    bb_wifi_prov_scan_wire_fill(&snap, NULL, 0, BB_WIFI_PROV_SCAN_NOT_STARTED);

    char buf[RENDER_BUF_BYTES];
    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&bb_wifi_prov_scan_wire_desc, &snap,
                                            buf, RENDER_BUF_BYTES, &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("{\"aps\":[],\"state\":\"not_started\"}", buf);
}

void test_wifi_prov_scan_wire_expected_json_one_ap_ready(void)
{
    bb_wifi_ap_t aps[1] = {
        { .ssid = "Solo", .rssi = -60, .secure = true },
    };

    bb_wifi_prov_scan_wire_t snap;
    bb_wifi_prov_scan_wire_fill(&snap, aps, 1, BB_WIFI_PROV_SCAN_READY);

    char buf[RENDER_BUF_BYTES];
    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&bb_wifi_prov_scan_wire_desc, &snap,
                                            buf, RENDER_BUF_BYTES, &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING(
        "{\"aps\":[{\"ssid\":\"Solo\",\"rssi\":-60,\"secure\":true}],\"state\":\"ready\"}", buf);
}

void test_wifi_prov_scan_wire_expected_json_pending(void)
{
    bb_wifi_prov_scan_wire_t snap;
    bb_wifi_prov_scan_wire_fill(&snap, NULL, 0, BB_WIFI_PROV_SCAN_PENDING);

    char buf[RENDER_BUF_BYTES];
    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&bb_wifi_prov_scan_wire_desc, &snap,
                                            buf, RENDER_BUF_BYTES, &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("{\"aps\":[],\"state\":\"pending\"}", buf);
}

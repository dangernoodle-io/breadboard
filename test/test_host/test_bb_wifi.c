#include "unity.h"
#include "bb_wifi.h"
#include <string.h>

#ifdef BB_WIFI_TESTING
#include "bb_wifi_test.h"
#endif

void test_bb_wifi_set_hostname_null(void)
{
    bb_err_t err = bb_wifi_set_hostname(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_wifi_set_hostname_empty(void)
{
    bb_err_t err = bb_wifi_set_hostname("");
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_wifi_set_hostname_valid(void)
{
    bb_err_t err = bb_wifi_set_hostname("valid-host");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

// Test: no-ip no-op
void test_bb_wifi_request_recovery_no_ip_noop(void)
{
#ifdef BB_WIFI_TESTING
    bb_wifi_test_reset_recovery();
    bb_wifi_test_set_has_ip(false);
    bb_err_t err = bb_wifi_request_recovery("test");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_wifi_test_get_recovery_count()); // no-op
#endif
}

// Test: has-ip triggers recovery
void test_bb_wifi_request_recovery_triggers_restart(void)
{
#ifdef BB_WIFI_TESTING
    bb_wifi_test_reset_recovery();
    bb_wifi_test_set_has_ip(true);
    bb_err_t err = bb_wifi_request_recovery("stratum_dead");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_wifi_test_get_recovery_count());
    TEST_ASSERT_NOT_NULL(bb_wifi_test_get_last_recovery_reason());
    TEST_ASSERT_EQUAL_STRING("stratum_dead", bb_wifi_test_get_last_recovery_reason());
#endif
}

// Test: null reason is safe
void test_bb_wifi_request_recovery_null_reason(void)
{
#ifdef BB_WIFI_TESTING
    bb_wifi_test_reset_recovery();
    bb_wifi_test_set_has_ip(true);
    bb_err_t err = bb_wifi_request_recovery(NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(1, bb_wifi_test_get_recovery_count());
#endif
}

// ---------------------------------------------------------------------------
// B1-411: restart_sta_count getter
// ---------------------------------------------------------------------------

// Host stub returns 0 by default.
void test_bb_wifi_restart_sta_count_default_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, bb_wifi_get_restart_sta_count());
}

// Test hook roundtrip: set a value, getter returns it.
void test_bb_wifi_restart_sta_count_test_hook_roundtrip(void)
{
#ifdef BB_WIFI_TESTING
    bb_wifi_test_set_restart_sta_count(7);
    TEST_ASSERT_EQUAL_UINT32(7, bb_wifi_get_restart_sta_count());
    bb_wifi_test_set_restart_sta_count(0);
#endif
}

// ---------------------------------------------------------------------------
// B1-411: disconnect_rssi getter
// ---------------------------------------------------------------------------

// Host stub returns INT8_MIN by default (no disconnect yet sentinel).
void test_bb_wifi_disconnect_rssi_default_sentinel(void)
{
    TEST_ASSERT_EQUAL_INT8(INT8_MIN, bb_wifi_get_disconnect_rssi());
}

// Test hook roundtrip: set a value, getter returns it.
void test_bb_wifi_disconnect_rssi_test_hook_roundtrip(void)
{
#ifdef BB_WIFI_TESTING
    bb_wifi_test_set_disconnect_rssi(-78);
    TEST_ASSERT_EQUAL_INT8(-78, bb_wifi_get_disconnect_rssi());
    bb_wifi_test_set_disconnect_rssi(INT8_MIN);
#endif
}

// ---------------------------------------------------------------------------
// B1-411: reason histogram host injection (BB_WIFI_TESTING)
// ---------------------------------------------------------------------------

// Inject a non-zero standard reason and verify top_reason via emit_section.
void test_bb_wifi_reason_histogram_inject_top_reason(void)
{
#ifdef BB_WIFI_TESTING
    uint16_t h[256];
    memset(h, 0, sizeof(h));
    h[3] = 5; // reason 3, count 5
    bb_wifi_test_set_reason_histogram(h, 256);

    uint16_t out[256];
    bb_wifi_get_reason_histogram(out, 256);
    TEST_ASSERT_EQUAL_UINT16(5, out[3]);
    TEST_ASSERT_EQUAL_UINT16(0, out[99]);

    // clean up
    bb_wifi_test_set_reason_histogram(NULL, 0);
#endif
}

// ---------------------------------------------------------------------------
// B1-411: reason histogram getter
// ---------------------------------------------------------------------------

// Host stub zeroes all buckets.
void test_bb_wifi_reason_histogram_host_returns_zeros(void)
{
    uint16_t hist[256];
    memset(hist, 0xFF, sizeof(hist));
    bb_wifi_get_reason_histogram(hist, 256);
    for (int i = 0; i < 256; i++) {
        TEST_ASSERT_EQUAL_UINT16(0, hist[i]);
    }
}

// NULL/zero-len calls must not crash.
void test_bb_wifi_reason_histogram_null_safe(void)
{
    bb_wifi_get_reason_histogram(NULL, 256);
    bb_wifi_get_reason_histogram(NULL, 0);
    uint16_t buf[4];
    bb_wifi_get_reason_histogram(buf, 0);
    // reaching here without crash is success
    TEST_PASS();
}

// ---------------------------------------------------------------------------
// B1-486: bb_wifi_reason_histogram_top (promoted from bb_net_health's
// private wifi_hist_priv.h to bb_wifi's public API, finding #1/#2)
// ---------------------------------------------------------------------------

// Inject a non-zero standard reason alongside non-zero sentinel buckets;
// the sentinels must be skipped and the standard reason reported as top.
void test_bb_wifi_reason_histogram_top_injected(void)
{
    uint16_t hist[256];
    memset(hist, 0, sizeof(hist));
    hist[3]                                   = 5;   // standard reason, top
    hist[BB_WIFI_REASON_BB_LOST_IP]           = 100; // sentinel — skipped
    hist[BB_WIFI_REASON_BB_EGRESS_DEAD]       = 200; // sentinel — skipped
    hist[BB_WIFI_REASON_BB_NO_IP_WATCHDOG]    = 300; // sentinel — skipped

    uint16_t top_count = 0;
    uint8_t  top_code  = bb_wifi_reason_histogram_top(hist, &top_count);
    TEST_ASSERT_EQUAL_UINT8(3, top_code);
    TEST_ASSERT_EQUAL_UINT16(5, top_count);
}

// All-zero histogram reports code 0 / count 0.
void test_bb_wifi_reason_histogram_top_all_zero(void)
{
    uint16_t hist[256];
    memset(hist, 0, sizeof(hist));

    uint16_t top_count = 1; // non-zero sentinel to prove it gets reset
    uint8_t  top_code  = bb_wifi_reason_histogram_top(hist, &top_count);
    TEST_ASSERT_EQUAL_UINT8(0, top_code);
    TEST_ASSERT_EQUAL_UINT16(0, top_count);
}

// NULL hist and/or NULL out_count must not crash.
void test_bb_wifi_reason_histogram_top_null_safe(void)
{
    uint16_t top_count = 99;
    uint8_t  top_code  = bb_wifi_reason_histogram_top(NULL, &top_count);
    TEST_ASSERT_EQUAL_UINT8(0, top_code);
    TEST_ASSERT_EQUAL_UINT16(0, top_count);

    uint16_t hist[256];
    memset(hist, 0, sizeof(hist));
    hist[5] = 1;
    top_code = bb_wifi_reason_histogram_top(hist, NULL);
    TEST_ASSERT_EQUAL_UINT8(5, top_code);

    // reaching here without crash is success
    TEST_PASS();
}

// ---------------------------------------------------------------------------
// B1-497: bb_wifi_is_roam pure predicate — OBSERVE-ONLY roam/BSSID-change
// detection. Exercises all three branches of the production STA_CONNECTED
// handler's roam decision without needing an ESP-IDF event loop.
// ---------------------------------------------------------------------------

// (a) BSSID changes while associated -> roam.
void test_bb_wifi_is_roam_bssid_changed(void)
{
    uint8_t prior[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t next[6]  = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    TEST_ASSERT_TRUE(bb_wifi_is_roam(prior, next));
}

// (b) same BSSID reconnect -> not a roam.
void test_bb_wifi_is_roam_same_bssid(void)
{
    uint8_t prior[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t next[6]  = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    TEST_ASSERT_FALSE(bb_wifi_is_roam(prior, next));
}

// (c) first connect since boot (all-zero prior BSSID) -> not a roam.
void test_bb_wifi_is_roam_first_connect_not_roam(void)
{
    uint8_t prior[6] = {0, 0, 0, 0, 0, 0};
    uint8_t next[6]  = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    TEST_ASSERT_FALSE(bb_wifi_is_roam(prior, next));
}

// NULL pointers must not crash and must report "not a roam".
void test_bb_wifi_is_roam_null_safe(void)
{
    uint8_t bssid[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    TEST_ASSERT_FALSE(bb_wifi_is_roam(NULL, bssid));
    TEST_ASSERT_FALSE(bb_wifi_is_roam(bssid, NULL));
    TEST_ASSERT_FALSE(bb_wifi_is_roam(NULL, NULL));
}

// ---------------------------------------------------------------------------
// B1-497: roam_count / roam_age_s getters
// ---------------------------------------------------------------------------

// Host stub returns 0 by default (no roam yet).
void test_bb_wifi_roam_count_default_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, bb_wifi_get_roam_count());
    TEST_ASSERT_EQUAL_UINT32(0, bb_wifi_get_roam_age_s());
}

// Test hook roundtrip: set values, getters return them.
void test_bb_wifi_roam_count_test_hook_roundtrip(void)
{
#ifdef BB_WIFI_TESTING
    bb_wifi_test_set_roam_count(4);
    bb_wifi_test_set_roam_age_s(120);
    TEST_ASSERT_EQUAL_UINT32(4, bb_wifi_get_roam_count());
    TEST_ASSERT_EQUAL_UINT32(120, bb_wifi_get_roam_age_s());
    bb_wifi_test_set_roam_count(0);
    bb_wifi_test_set_roam_age_s(0);
#endif
}

// ---------------------------------------------------------------------------
// wifi-netmode PR: bb_wifi_is_associated accessor
// ---------------------------------------------------------------------------

// Host stub returns false by default.
void test_bb_wifi_is_associated_default_false(void)
{
    TEST_ASSERT_FALSE(bb_wifi_is_associated());
}

// Test hook roundtrip: set true/false, getter reflects it.
void test_bb_wifi_is_associated_test_hook_roundtrip(void)
{
#ifdef BB_WIFI_TESTING
    bb_wifi_test_set_associated(true);
    TEST_ASSERT_TRUE(bb_wifi_is_associated());
    bb_wifi_test_set_associated(false);
    TEST_ASSERT_FALSE(bb_wifi_is_associated());
#endif
}

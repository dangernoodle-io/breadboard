#include "unity.h"
#include "bb_wifi.h"
#include <string.h>

#ifdef BB_WIFI_TESTING
#include "bb_wifi_test.h"
#endif

// bb_wifi_internal_ota_validated / bb_wifi_on_disconnect_invoke are the
// private accessors declared in platform/espidf/bb_wifi/wifi_reconn.h
// (bb_wifi.c / wifi_reconn.c's own copy) -- mirrored here since that
// directory isn't on this file's include path in the host build. Real,
// shipped functions (defined via BB_CALLBACK_SLOT_* in
// platform/host/bb_wifi/bb_wifi_emit.c, host-compiled).
bool bb_wifi_internal_ota_validated(void);
void bb_wifi_on_disconnect_invoke(void);
void bb_wifi_net_event_invoke(bb_wifi_net_event_t evt, bb_wifi_disc_reason_t reason);

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
    uint16_t h[BB_WIFI_DISC_COUNT];
    memset(h, 0, sizeof(h));
    h[BB_WIFI_DISC_HANDSHAKE_TIMEOUT] = 5;
    bb_wifi_test_set_reason_histogram(h, BB_WIFI_DISC_COUNT);

    uint16_t out[BB_WIFI_DISC_COUNT];
    bb_wifi_get_reason_histogram(out, BB_WIFI_DISC_COUNT);
    TEST_ASSERT_EQUAL_UINT16(5, out[BB_WIFI_DISC_HANDSHAKE_TIMEOUT]);
    TEST_ASSERT_EQUAL_UINT16(0, out[BB_WIFI_DISC_BB_LOST_IP]);

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
    uint16_t hist[BB_WIFI_DISC_COUNT];
    memset(hist, 0xFF, sizeof(hist));
    bb_wifi_get_reason_histogram(hist, BB_WIFI_DISC_COUNT);
    for (int i = 0; i < BB_WIFI_DISC_COUNT; i++) {
        TEST_ASSERT_EQUAL_UINT16(0, hist[i]);
    }
}

// NULL/zero-len calls must not crash.
void test_bb_wifi_reason_histogram_null_safe(void)
{
    bb_wifi_get_reason_histogram(NULL, BB_WIFI_DISC_COUNT);
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

// Inject a non-zero standard reason alongside non-zero breadboard-injected
// buckets; those must be skipped and the standard reason reported as top.
void test_bb_wifi_reason_histogram_top_injected(void)
{
    uint16_t hist[BB_WIFI_DISC_COUNT];
    memset(hist, 0, sizeof(hist));
    hist[BB_WIFI_DISC_INACTIVITY]          = 5;   // standard reason, top
    hist[BB_WIFI_REASON_BB_LOST_IP]        = 100; // breadboard-injected — skipped
    hist[BB_WIFI_REASON_BB_EGRESS_DEAD]    = 200; // breadboard-injected — skipped
    hist[BB_WIFI_REASON_BB_NO_IP_WATCHDOG] = 300; // breadboard-injected — skipped

    uint16_t top_count = 0;
    bb_wifi_disc_reason_t top_code = bb_wifi_reason_histogram_top(hist, &top_count);
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_INACTIVITY, top_code);
    TEST_ASSERT_EQUAL_UINT16(5, top_count);
}

// All-zero histogram reports BB_WIFI_DISC_UNKNOWN / count 0.
void test_bb_wifi_reason_histogram_top_all_zero(void)
{
    uint16_t hist[BB_WIFI_DISC_COUNT];
    memset(hist, 0, sizeof(hist));

    uint16_t top_count = 1; // non-zero sentinel to prove it gets reset
    bb_wifi_disc_reason_t top_code = bb_wifi_reason_histogram_top(hist, &top_count);
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_UNKNOWN, top_code);
    TEST_ASSERT_EQUAL_UINT16(0, top_count);
}

// NULL hist and/or NULL out_count must not crash.
void test_bb_wifi_reason_histogram_top_null_safe(void)
{
    uint16_t top_count = 99;
    bb_wifi_disc_reason_t top_code = bb_wifi_reason_histogram_top(NULL, &top_count);
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_UNKNOWN, top_code);
    TEST_ASSERT_EQUAL_UINT16(0, top_count);

    uint16_t hist[BB_WIFI_DISC_COUNT];
    memset(hist, 0, sizeof(hist));
    hist[BB_WIFI_DISC_NO_AP_FOUND] = 1;
    top_code = bb_wifi_reason_histogram_top(hist, NULL);
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_NO_AP_FOUND, top_code);

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

// ---------------------------------------------------------------------------
// wifi-drop-log PR: bb_wifi_get_last_session_s getter
// ---------------------------------------------------------------------------

// Host stub returns 0 by default (no session has ended yet).
void test_bb_wifi_last_session_s_default_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, bb_wifi_get_last_session_s());
}

// Test hook roundtrip: set a value, getter returns it.
void test_bb_wifi_last_session_s_test_hook_roundtrip(void)
{
#ifdef BB_WIFI_TESTING
    bb_wifi_test_set_last_session_s(137);
    TEST_ASSERT_EQUAL_UINT32(137, bb_wifi_get_last_session_s());
    bb_wifi_test_set_last_session_s(0);
#endif
}

// ---------------------------------------------------------------------------
// KB 820 (bb_wifi reason contract, PR1): bb_wifi_disc_reason_str — every
// enum member's label + an out-of-range default.
// ---------------------------------------------------------------------------

void test_bb_wifi_disc_reason_str_unknown(void)
{
    TEST_ASSERT_EQUAL_STRING("unknown", bb_wifi_disc_reason_str(BB_WIFI_DISC_UNKNOWN));
}

void test_bb_wifi_disc_reason_str_auth_fail(void)
{
    TEST_ASSERT_EQUAL_STRING("auth_fail", bb_wifi_disc_reason_str(BB_WIFI_DISC_AUTH_FAIL));
}

void test_bb_wifi_disc_reason_str_assoc_fail(void)
{
    TEST_ASSERT_EQUAL_STRING("assoc_fail", bb_wifi_disc_reason_str(BB_WIFI_DISC_ASSOC_FAIL));
}

void test_bb_wifi_disc_reason_str_handshake_timeout(void)
{
    TEST_ASSERT_EQUAL_STRING("handshake_timeout", bb_wifi_disc_reason_str(BB_WIFI_DISC_HANDSHAKE_TIMEOUT));
}

void test_bb_wifi_disc_reason_str_connection_lost(void)
{
    TEST_ASSERT_EQUAL_STRING("connection_lost", bb_wifi_disc_reason_str(BB_WIFI_DISC_CONNECTION_LOST));
}

void test_bb_wifi_disc_reason_str_no_ap_found(void)
{
    TEST_ASSERT_EQUAL_STRING("no_ap_found", bb_wifi_disc_reason_str(BB_WIFI_DISC_NO_AP_FOUND));
}

void test_bb_wifi_disc_reason_str_inactivity(void)
{
    TEST_ASSERT_EQUAL_STRING("inactivity", bb_wifi_disc_reason_str(BB_WIFI_DISC_INACTIVITY));
}

void test_bb_wifi_disc_reason_str_deauth(void)
{
    TEST_ASSERT_EQUAL_STRING("deauth", bb_wifi_disc_reason_str(BB_WIFI_DISC_DEAUTH));
}

void test_bb_wifi_disc_reason_str_beacon_timeout(void)
{
    TEST_ASSERT_EQUAL_STRING("beacon_timeout", bb_wifi_disc_reason_str(BB_WIFI_DISC_BEACON_TIMEOUT));
}

void test_bb_wifi_disc_reason_str_bb_lost_ip(void)
{
    TEST_ASSERT_EQUAL_STRING("bb_lost_ip", bb_wifi_disc_reason_str(BB_WIFI_REASON_BB_LOST_IP));
}

void test_bb_wifi_disc_reason_str_bb_egress_dead(void)
{
    TEST_ASSERT_EQUAL_STRING("bb_egress_dead", bb_wifi_disc_reason_str(BB_WIFI_REASON_BB_EGRESS_DEAD));
}

void test_bb_wifi_disc_reason_str_bb_no_ip_watchdog(void)
{
    TEST_ASSERT_EQUAL_STRING("bb_no_ip_watchdog", bb_wifi_disc_reason_str(BB_WIFI_REASON_BB_NO_IP_WATCHDOG));
}

// Out-of-range value falls through to the default "unknown" arm — every
// return is a static literal, no shared mutable buffer, never NULL.
void test_bb_wifi_disc_reason_str_default_out_of_range(void)
{
    TEST_ASSERT_EQUAL_STRING("unknown", bb_wifi_disc_reason_str((bb_wifi_disc_reason_t)999));
}

// ---------------------------------------------------------------------------
// KB 820 (bb_wifi reason contract, PR1): bb_wifi_map_esp_reason — every
// mapped esp_wifi WIFI_REASON_* code + default.
// ---------------------------------------------------------------------------

void test_bb_wifi_map_esp_reason_auth_expire(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_AUTH_FAIL, bb_wifi_map_esp_reason(2));
}

void test_bb_wifi_map_esp_reason_auth_leave(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_DEAUTH, bb_wifi_map_esp_reason(3));
}

void test_bb_wifi_map_esp_reason_disassoc_inactivity(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_INACTIVITY, bb_wifi_map_esp_reason(4));
}

void test_bb_wifi_map_esp_reason_4way_handshake_timeout(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_HANDSHAKE_TIMEOUT, bb_wifi_map_esp_reason(15));
}

void test_bb_wifi_map_esp_reason_beacon_timeout(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_BEACON_TIMEOUT, bb_wifi_map_esp_reason(200));
}

void test_bb_wifi_map_esp_reason_no_ap_found(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_NO_AP_FOUND, bb_wifi_map_esp_reason(201));
}

void test_bb_wifi_map_esp_reason_auth_fail(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_AUTH_FAIL, bb_wifi_map_esp_reason(202));
}

void test_bb_wifi_map_esp_reason_assoc_fail(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_ASSOC_FAIL, bb_wifi_map_esp_reason(203));
}

void test_bb_wifi_map_esp_reason_handshake_timeout(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_HANDSHAKE_TIMEOUT, bb_wifi_map_esp_reason(204));
}

void test_bb_wifi_map_esp_reason_connection_fail(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_CONNECTION_LOST, bb_wifi_map_esp_reason(205));
}

// Sanity scan (firmware-review finding): the NO_AP_FOUND_* variants (210-212)
// are unambiguously "no AP found" by name -- map them into the same bucket
// as 201 rather than leaving them BB_WIFI_DISC_UNKNOWN.
void test_bb_wifi_map_esp_reason_no_ap_found_w_compatible_security(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_NO_AP_FOUND, bb_wifi_map_esp_reason(210));
}

void test_bb_wifi_map_esp_reason_no_ap_found_in_authmode_threshold(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_NO_AP_FOUND, bb_wifi_map_esp_reason(211));
}

void test_bb_wifi_map_esp_reason_no_ap_found_in_rssi_threshold(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_NO_AP_FOUND, bb_wifi_map_esp_reason(212));
}

void test_bb_wifi_map_esp_reason_default_unmapped(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_UNKNOWN, bb_wifi_map_esp_reason(99));
}

// ---------------------------------------------------------------------------
// KB 820 (bb_wifi reason contract, PR1): bb_wifi_map_wl_status — every
// mapped Arduino WiFiS3 wl_status_t code + default.
// ---------------------------------------------------------------------------

void test_bb_wifi_map_wl_status_connection_lost(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_CONNECTION_LOST, bb_wifi_map_wl_status(5)); // WL_CONNECTION_LOST
}

void test_bb_wifi_map_wl_status_connect_failed(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_ASSOC_FAIL, bb_wifi_map_wl_status(4)); // WL_CONNECT_FAILED
}

void test_bb_wifi_map_wl_status_no_ssid_avail(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_NO_AP_FOUND, bb_wifi_map_wl_status(1)); // WL_NO_SSID_AVAIL
}

void test_bb_wifi_map_wl_status_default_unmapped(void)
{
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_UNKNOWN, bb_wifi_map_wl_status(3)); // WL_CONNECTED (not a disconnect code)
}

// ---------------------------------------------------------------------------
// B1-518 PR2: bb_wifi_get_gateway_status accessor (observe-only gateway
// probe). Host stub always returns BB_OK; the "never ran" BB_ERR_INVALID_STATE
// branch is ESP-IDF-only (before bb_wifi_gw_probe_start runs) and is not
// reachable from the host build.
// ---------------------------------------------------------------------------

// NULL out -> BB_ERR_INVALID_ARG.
void test_bb_wifi_get_gateway_status_null_arg(void)
{
    bb_err_t err = bb_wifi_get_gateway_status(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

// Default (no test hook driven): BB_OK, zeroed status, gw_reachable=false.
void test_bb_wifi_get_gateway_status_default_zeroed(void)
{
#ifdef BB_WIFI_TESTING
    bb_wifi_host_set_gateway_status(NULL); // ensure clean default
#endif
    bb_wifi_gw_status_t st;
    memset(&st, 0xAA, sizeof(st)); // poison to prove the accessor overwrites it
    bb_err_t err = bb_wifi_get_gateway_status(&st);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_FALSE(st.gw_reachable);
    TEST_ASSERT_EQUAL_UINT8(0, st.gw_fail_streak);
    TEST_ASSERT_EQUAL_UINT32(0, st.gw_probe_count);
    TEST_ASSERT_EQUAL_UINT32(0, st.gw_fail_count);
    TEST_ASSERT_EQUAL_UINT32(0, st.gw_dead_count);
    TEST_ASSERT_EQUAL_UINT64(0, st.last_gw_probe_ms);
}

// bb_wifi_host_set_gateway_status hook roundtrip: set a status, getter
// returns it verbatim; NULL clears back to the default zeroed status.
void test_bb_wifi_get_gateway_status_test_hook_roundtrip(void)
{
#ifdef BB_WIFI_TESTING
    bb_wifi_gw_status_t in = {
        .gw_reachable = true,
        .gw_fail_streak = 2,
        .gw_probe_count = 42,
        .gw_fail_count = 5,
        .gw_dead_count = 3,
        .last_gw_probe_ms = 123456789ULL,
    };
    bb_wifi_host_set_gateway_status(&in);

    bb_wifi_gw_status_t out;
    bb_err_t err = bb_wifi_get_gateway_status(&out);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_TRUE(out.gw_reachable);
    TEST_ASSERT_EQUAL_UINT8(2, out.gw_fail_streak);
    TEST_ASSERT_EQUAL_UINT32(42, out.gw_probe_count);
    TEST_ASSERT_EQUAL_UINT32(5, out.gw_fail_count);
    TEST_ASSERT_EQUAL_UINT32(3, out.gw_dead_count);
    TEST_ASSERT_EQUAL_UINT64(123456789ULL, out.last_gw_probe_ms);

    bb_wifi_host_set_gateway_status(NULL);
    err = bb_wifi_get_gateway_status(&out);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_FALSE(out.gw_reachable);
#endif
}

// ---------------------------------------------------------------------------
// Callback-slot real instantiations (bb_wifi_emit.c) -- the SHIPPED
// setter/invoke pairs, now host-compiled (components/bb_wifi/CMakeLists.txt
// puts platform/host/bb_wifi/bb_wifi_emit.c in both the espidf SRCS and the
// host build), so these exercise the exact production functions rather than
// a per-seam copy. The macro's generic fire/no-fire logic itself is covered
// once, in isolation, by test/test_host/test_bb_callback_slot.c.
// ---------------------------------------------------------------------------

// ota-validated (BB_CALLBACK_SLOT_RET): default true when unset, echoes the
// callback's value once set.
void test_bb_wifi_ota_validated_default_true_when_unset(void)
{
    bb_wifi_set_ota_validated_cb(NULL);
    TEST_ASSERT_TRUE(bb_wifi_internal_ota_validated());
}

static bool s_ota_validated_fixture_value = false;
static bool ota_validated_fixture(void) { return s_ota_validated_fixture_value; }

void test_bb_wifi_ota_validated_set_cb_returns_value(void)
{
    s_ota_validated_fixture_value = false;
    bb_wifi_set_ota_validated_cb(ota_validated_fixture);
    TEST_ASSERT_FALSE(bb_wifi_internal_ota_validated());

    s_ota_validated_fixture_value = true;
    TEST_ASSERT_TRUE(bb_wifi_internal_ota_validated());

    bb_wifi_set_ota_validated_cb(NULL);
}

// on_disconnect (BB_CALLBACK_SLOT_VOID0): null slot is a no-op; set slot
// fires on invoke.
static int s_on_disconnect_calls = 0;
static void on_disconnect_fixture(void) { s_on_disconnect_calls++; }

void test_bb_wifi_on_disconnect_null_is_noop(void)
{
    bb_wifi_register_on_disconnect(NULL);
    s_on_disconnect_calls = 0;
    bb_wifi_on_disconnect_invoke();
    TEST_ASSERT_EQUAL_INT(0, s_on_disconnect_calls);
}

void test_bb_wifi_on_disconnect_set_cb_is_invoked(void)
{
    bb_wifi_register_on_disconnect(on_disconnect_fixture);
    s_on_disconnect_calls = 0;
    bb_wifi_on_disconnect_invoke();
    TEST_ASSERT_EQUAL_INT(1, s_on_disconnect_calls);
    bb_wifi_register_on_disconnect(NULL);
}

// net_event (BB_CALLBACK_SLOT_VOID, with-args): null sink is a no-op; set
// sink dispatches the invoked enum value through to the sink.
static int s_net_event_calls = 0;
static bb_wifi_net_event_t s_net_event_last_evt;
static bb_wifi_disc_reason_t s_net_event_last_reason;
static void net_event_fixture(bb_wifi_net_event_t evt, bb_wifi_disc_reason_t reason)
{
    s_net_event_calls++;
    s_net_event_last_evt = evt;
    s_net_event_last_reason = reason;
}

void test_bb_wifi_net_event_null_sink_is_noop(void)
{
    bb_wifi_set_net_event_sink(NULL);
    s_net_event_calls = 0;
    bb_wifi_net_event_invoke(BB_WIFI_NET_EVT_GOT_IP, BB_WIFI_DISC_UNKNOWN);
    TEST_ASSERT_EQUAL_INT(0, s_net_event_calls);
}

// GOT_IP carries BB_WIFI_DISC_UNKNOWN (not a meaningful reason).
void test_bb_wifi_net_event_set_sink_dispatches_got_ip(void)
{
    bb_wifi_set_net_event_sink(net_event_fixture);
    s_net_event_calls = 0;
    bb_wifi_net_event_invoke(BB_WIFI_NET_EVT_GOT_IP, BB_WIFI_DISC_UNKNOWN);
    TEST_ASSERT_EQUAL_INT(1, s_net_event_calls);
    TEST_ASSERT_EQUAL_INT(BB_WIFI_NET_EVT_GOT_IP, s_net_event_last_evt);
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_UNKNOWN, s_net_event_last_reason);
    bb_wifi_set_net_event_sink(NULL);
}

// DISCONNECT carries the mapped reason through as an explicit argument
// (the staleness-race fix) -- production passes bb_wifi_map_esp_reason()'s
// result; here a distinct reason from GOT_IP/LOST_IP's fixed values proves
// it's a passthrough, not a hardcoded constant.
void test_bb_wifi_net_event_set_sink_dispatches_disconnect(void)
{
    bb_wifi_set_net_event_sink(net_event_fixture);
    s_net_event_calls = 0;
    bb_wifi_net_event_invoke(BB_WIFI_NET_EVT_DISCONNECT, BB_WIFI_DISC_AUTH_FAIL);
    TEST_ASSERT_EQUAL_INT(1, s_net_event_calls);
    TEST_ASSERT_EQUAL_INT(BB_WIFI_NET_EVT_DISCONNECT, s_net_event_last_evt);
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_AUTH_FAIL, s_net_event_last_reason);
    bb_wifi_set_net_event_sink(NULL);
}

// LOST_IP always carries BB_WIFI_DISC_BB_LOST_IP.
void test_bb_wifi_net_event_set_sink_dispatches_lost_ip(void)
{
    bb_wifi_set_net_event_sink(net_event_fixture);
    s_net_event_calls = 0;
    bb_wifi_net_event_invoke(BB_WIFI_NET_EVT_LOST_IP, BB_WIFI_DISC_BB_LOST_IP);
    TEST_ASSERT_EQUAL_INT(1, s_net_event_calls);
    TEST_ASSERT_EQUAL_INT(BB_WIFI_NET_EVT_LOST_IP, s_net_event_last_evt);
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_BB_LOST_IP, s_net_event_last_reason);
    bb_wifi_set_net_event_sink(NULL);
}

// bb_wifi_event_payload_build (KB 820 PR2) -- pure, host-testable builder.
void test_bb_wifi_event_payload_build_null_out_is_noop(void)
{
    bb_wifi_event_payload_build(NULL, BB_WIFI_NET_EVT_GOT_IP, BB_WIFI_DISC_UNKNOWN, "10.0.0.5");
    TEST_PASS();
}

void test_bb_wifi_event_payload_build_got_ip_populates_ip(void)
{
    bb_wifi_event_payload_t payload;
    memset(&payload, 0xAA, sizeof(payload));
    bb_wifi_event_payload_build(&payload, BB_WIFI_NET_EVT_GOT_IP, BB_WIFI_DISC_UNKNOWN, "10.0.0.5");
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", payload.ip);
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_UNKNOWN, payload.disc_reason);
}

void test_bb_wifi_event_payload_build_got_ip_null_ip_blanks(void)
{
    bb_wifi_event_payload_t payload;
    memset(&payload, 0xAA, sizeof(payload));
    bb_wifi_event_payload_build(&payload, BB_WIFI_NET_EVT_GOT_IP, BB_WIFI_DISC_UNKNOWN, NULL);
    TEST_ASSERT_EQUAL_STRING("", payload.ip);
}

// Blanking enforcement: a non-GOT_IP evt never carries an ip through, even
// if the caller passes a stray non-NULL ip string.
void test_bb_wifi_event_payload_build_disconnect_blanks_ip(void)
{
    bb_wifi_event_payload_t payload;
    memset(&payload, 0xAA, sizeof(payload));
    bb_wifi_event_payload_build(&payload, BB_WIFI_NET_EVT_DISCONNECT, BB_WIFI_DISC_AUTH_FAIL, "9.9.9.9");
    TEST_ASSERT_EQUAL_STRING("", payload.ip);
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_AUTH_FAIL, payload.disc_reason);
}

void test_bb_wifi_event_payload_build_lost_ip_blanks_ip(void)
{
    bb_wifi_event_payload_t payload;
    memset(&payload, 0xAA, sizeof(payload));
    bb_wifi_event_payload_build(&payload, BB_WIFI_NET_EVT_LOST_IP, BB_WIFI_DISC_BB_LOST_IP, NULL);
    TEST_ASSERT_EQUAL_STRING("", payload.ip);
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_BB_LOST_IP, payload.disc_reason);
}

// reason passthrough: an arbitrary reason value survives verbatim.
void test_bb_wifi_event_payload_build_reason_passthrough(void)
{
    bb_wifi_event_payload_t payload;
    memset(&payload, 0xAA, sizeof(payload));
    bb_wifi_event_payload_build(&payload, BB_WIFI_NET_EVT_DISCONNECT, BB_WIFI_DISC_BEACON_TIMEOUT, NULL);
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_BEACON_TIMEOUT, payload.disc_reason);
}

// ---------------------------------------------------------------------------
// Host-stub NULL-arg branches (bb_wifi_host.c) -- coverage-filter finding
// (PR1): platform/host/bb_wifi/ is now graded, surfacing these previously
// untested guard branches.
// ---------------------------------------------------------------------------

void test_bb_wifi_get_info_null_arg(void)
{
    bb_err_t err = bb_wifi_get_info(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_wifi_get_info_zeroed_snapshot(void)
{
    bb_wifi_info_t info;
    memset(&info, 0xAA, sizeof(info));
    bb_err_t err = bb_wifi_get_info(&info);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("0.0.0.0", info.ip);
}

// Both out-params NULL is safe (no-op).
void test_bb_wifi_get_disconnect_both_null_safe(void)
{
    bb_wifi_get_disconnect(NULL, NULL);
    TEST_PASS();
}

// Both out-params non-NULL: written with the host-stub defaults.
void test_bb_wifi_get_disconnect_writes_both_out_params(void)
{
    bb_wifi_disc_reason_t reason = BB_WIFI_DISC_AUTH_FAIL;
    int64_t age_us = -1;
    bb_wifi_get_disconnect(&reason, &age_us);
    TEST_ASSERT_EQUAL(BB_WIFI_DISC_UNKNOWN, reason);
    TEST_ASSERT_EQUAL_INT64(0, age_us);
}

void test_bb_wifi_get_ip_str_null_out(void)
{
    bb_err_t err = bb_wifi_get_ip_str(NULL, 16);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_wifi_get_ip_str_zero_len(void)
{
    char buf[16];
    bb_err_t err = bb_wifi_get_ip_str(buf, 0);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_wifi_get_ip_str_valid(void)
{
    char buf[16];
    bb_err_t err = bb_wifi_get_ip_str(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("0.0.0.0", buf);
}

void test_bb_wifi_get_rssi_null_out(void)
{
    bb_err_t err = bb_wifi_get_rssi(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_wifi_get_rssi_valid(void)
{
    int8_t rssi = -1;
    bb_err_t err = bb_wifi_get_rssi(&rssi);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_INT8(0, rssi);
}

// bb_wifi_test_set_recovery_blocked's true branch: recovery is suppressed
// even though has_ip is set (the "blocked" no-op path).
void test_bb_wifi_request_recovery_blocked_noop(void)
{
#ifdef BB_WIFI_TESTING
    bb_wifi_test_reset_recovery();
    bb_wifi_test_set_has_ip(true);
    bb_wifi_test_set_recovery_blocked(true);
    bb_err_t err = bb_wifi_request_recovery("blocked_reason");
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(0, bb_wifi_test_get_recovery_count());
    bb_wifi_test_set_recovery_blocked(false);
#endif
}

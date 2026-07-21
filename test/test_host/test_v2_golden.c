// test_v2_golden — byte-fidelity harness for the v2 wire descriptors
// (B1-767 PR-6): a synthetic bb_serialize worked-example fixture, health
// root-identity slice.
//
// widget fixture: byte-fidelity coverage for a purely synthetic descriptor
// (test_serialize_fixture.h) -- not a spec for any production payload, just
// exercise of the string/bool/i64/str_n/arr_str field-type diversity.
//
// health (root-identity slice): each golden string is the SPEC for the
// ROOT-IDENTITY FIELD SLICE ONLY -- the fields the ESP-IDF /api/health
// handler gathers directly (bb_wifi/bb_mdns) and hands to
// bb_health_compose_and_stream() as its RAW group. A mismatch means the
// DESCRIPTOR (order/type/max_len/present) is wrong for that slice -- fix
// the descriptor, never the golden.
//
// health (full-document golden, B1-1100 note 2): the trailing section below
// asserts byte-exact output of the FULL composed /api/health document
// (root + "temp" + "mqtt") via bb_health_compose_and_stream() itself --
// registering the REAL bb_temp/bb_mqtt_client producers, not synthetic
// fixtures -- the host-side proxy for the user's on-device curl check
// (bb_health_compose_and_stream() is not host-reproducible for the ROOT
// gather, since bb_mdns_get_hostname() is ESP_PLATFORM-only; the root
// snapshot below is hand-filled with the same field values the ESP-IDF
// handler would have gathered). validated dropped (B1-977, bb_board
// dissolution).
//
// The info-telemetry envelope golden (which mirrored
// platform/host/bb_pub_info/bb_pub_info.c's info_serialize()) was removed
// with the bb_pub/bb_sink_* cluster cut (B1-905) -- bb_pub_info no longer
// exists, so there is no live path left to spec or diff against.

#include "unity.h"

#include "bb_serialize_json.h"
#include "bb_http_host.h"
#include "bb_mqtt_client.h"
#include "bb_temp.h"
#include "bb_temp_test.h"
#include "bb_wifi.h"
#include "bb_wifi_test.h"

#include "test_serialize_fixture.h"
#include "../../components/bb_health/bb_health_wire_priv.h"
#include "../../components/bb_health/bb_health_compose_priv.h"
#include "../../components/bb_wifi_http/bb_wifi_http_wire_priv.h"

#include <string.h>
#include <stdio.h>

static void render_eq(const bb_serialize_desc_t *d, const void *snap, const char *golden)
{
    char   buf[512];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_serialize_json_render(d, snap, buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL_STRING(golden, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(golden), n);
}

// ---------------------------------------------------------------------------
// widget fixture (synthetic -- see test_serialize_fixture.h)
// ---------------------------------------------------------------------------

void test_v2_golden_widget_fixture_populated(void)
{
    test_fixture_widget_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.serial, "widget-serial-001", sizeof(snap.serial) - 1);
    snap.calibrated        = true;
    snap.armed              = true;
    snap.installed_epoch_s  = 1704067200;
    strncpy(snap.region, "rg-a", sizeof(snap.region) - 1);
    snap.label = (bb_serialize_str_n_t){ .ptr = "widget-01", .len = 9 };

    const char *tags[] = { "alpha", "beta" };
    snap.tags = (bb_serialize_arr_str_t){ .items = tags, .count = 2 };

    render_eq(&bb_fixture_widget_desc, &snap,
              "{\"serial\":\"widget-serial-001\",\"calibrated\":true,\"armed\":true,"
              "\"installed_epoch_s\":1704067200,\"region\":\"rg-a\",\"label\":\"widget-01\","
              "\"tags\":[\"alpha\",\"beta\"]}");
}

void test_v2_golden_widget_fixture_null(void)
{
    test_fixture_widget_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.region, "unset", sizeof(snap.region) - 1);
    snap.label = (bb_serialize_str_n_t){ .ptr = NULL, .len = 0 };
    snap.tags  = (bb_serialize_arr_str_t){ .items = NULL, .count = 0 };

    render_eq(&bb_fixture_widget_desc, &snap,
              "{\"serial\":\"\",\"calibrated\":false,\"armed\":false,"
              "\"installed_epoch_s\":0,\"region\":\"unset\",\"label\":null,"
              "\"tags\":[]}");
}

// ---------------------------------------------------------------------------
// widget fixture -- boundary / insurance goldens
// ---------------------------------------------------------------------------

// Additional insurance golden: array-walk loop coverage.
void test_v2_golden_widget_fixture_tags_three_entries(void)
{
    test_fixture_widget_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.region, "unset", sizeof(snap.region) - 1);
    snap.label = (bb_serialize_str_n_t){ .ptr = NULL, .len = 0 };

    const char *tags[] = { "alpha", "beta", "gamma" };
    snap.tags = (bb_serialize_arr_str_t){ .items = tags, .count = 3 };

    render_eq(&bb_fixture_widget_desc, &snap,
              "{\"serial\":\"\",\"calibrated\":false,\"armed\":false,"
              "\"installed_epoch_s\":0,\"region\":\"unset\",\"label\":null,"
              "\"tags\":[\"alpha\",\"beta\",\"gamma\"]}");
}

// (b) serial[18] filled to its exact max_len with NO NUL terminator --
// exercises the BB_TYPE_STR strnlen(s, max_len) cap: the walker must yield
// exactly 18 bytes, never read past the array end looking for a NUL that
// isn't there.
void test_v2_golden_widget_fixture_serial_boundary_no_nul(void)
{
    test_fixture_widget_t snap;
    memset(&snap, 0, sizeof(snap));
    // Exactly 18 bytes, no NUL -- fills test_fixture_widget_t.serial (char[18])
    // to capacity.
    memcpy(snap.serial, "112233445566778899", sizeof(snap.serial));
    strncpy(snap.region, "unset", sizeof(snap.region) - 1);
    snap.label = (bb_serialize_str_n_t){ .ptr = NULL, .len = 0 };
    snap.tags  = (bb_serialize_arr_str_t){ .items = NULL, .count = 0 };

    render_eq(&bb_fixture_widget_desc, &snap,
              "{\"serial\":\"112233445566778899\",\"calibrated\":false,\"armed\":false,"
              "\"installed_epoch_s\":0,\"region\":\"unset\",\"label\":null,"
              "\"tags\":[]}");
}

// Additional insurance golden: unbounded STR_N path coverage.
void test_v2_golden_widget_fixture_label_longer_str_n(void)
{
    test_fixture_widget_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.region, "unset", sizeof(snap.region) - 1);
    static const char label[] = "widget-fixture-long-label-01";
    snap.label = (bb_serialize_str_n_t){ .ptr = label, .len = sizeof(label) - 1 };
    snap.tags  = (bb_serialize_arr_str_t){ .items = NULL, .count = 0 };

    render_eq(&bb_fixture_widget_desc, &snap,
              "{\"serial\":\"\",\"calibrated\":false,\"armed\":false,"
              "\"installed_epoch_s\":0,\"region\":\"unset\","
              "\"label\":\"widget-fixture-long-label-01\",\"tags\":[]}");
}

// ---------------------------------------------------------------------------
// health (root-identity slice)
// ---------------------------------------------------------------------------

void test_v2_golden_health_root_slice_populated(void)
{
    bb_health_wire_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.ok        = true;
    strncpy(snap.network.ssid, "testnet", sizeof(snap.network.ssid) - 1);
    strncpy(snap.network.bssid, "aa:bb:cc:dd:ee:ff", sizeof(snap.network.bssid) - 1);
    strncpy(snap.network.ip, "192.168.1.50", sizeof(snap.network.ip) - 1);
    snap.network.connected = true;
    snap.network.mdns = (bb_serialize_str_n_t){ .ptr = "bb-test", .len = 7 };

    render_eq(&bb_health_wire_desc, &snap,
              "{\"ok\":true,\"network\":{\"ssid\":\"testnet\","
              "\"bssid\":\"aa:bb:cc:dd:ee:ff\",\"ip\":\"192.168.1.50\","
              "\"connected\":true,\"mdns\":\"bb-test\"}}");
}

void test_v2_golden_health_root_slice_disconnected(void)
{
    bb_health_wire_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.network.bssid, "00:00:00:00:00:00", sizeof(snap.network.bssid) - 1);
    strncpy(snap.network.ip, "0.0.0.0", sizeof(snap.network.ip) - 1);
    snap.network.mdns = (bb_serialize_str_n_t){ .ptr = NULL, .len = 0 };

    render_eq(&bb_health_wire_desc, &snap,
              "{\"ok\":false,\"network\":{\"ssid\":\"\","
              "\"bssid\":\"00:00:00:00:00:00\",\"ip\":\"0.0.0.0\","
              "\"connected\":false,\"mdns\":null}}");
}

// ---------------------------------------------------------------------------
// health FULL-DOCUMENT golden (B1-1100 note 2) -- root + "temp" + "mqtt",
// composed via bb_health_compose_and_stream() itself (the same portable seam
// the ESP-IDF /api/health handler calls), registering the REAL bb_temp/
// bb_mqtt_client producers -- not synthetic fixtures. This is the host-side
// proxy for the user's on-device curl check: byte-exact against this golden
// here means byte-exact against today's live /api/health there too, modulo
// the ROOT gather itself (bb_wifi/bb_mdns), which is ESP_PLATFORM-only and
// hand-filled below with the same field values/shapes the ESP-IDF handler
// would have gathered.
//
// f64_shortest proof: bb_temp_test_set_soc(true, 55.3f) drives soc_c through
// the SAME rounding bb_temp_health_fill() applies in production, then renders
// via bb_health_compose_and_stream()'s f64_shortest=true path -- "55.3", not
// the fixed-decimal "55.300000" a reverted flag would produce.
// ---------------------------------------------------------------------------

void test_v2_golden_health_full_document(void)
{
    // Register the REAL "temp" and "mqtt" producers (production fns, not
    // fixtures) -- setUp()'s bb_health_reset_for_test() has already cleared
    // the registry for this test.
    bb_temp_test_set_soc(true, 55.3f);
    bb_temp_register_info();

    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://broker.example.com:1883" };
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);
    bb_mqtt_client_health_register();

    bb_health_wire_t root;
    memset(&root, 0, sizeof(root));
    root.ok        = true;
    strncpy(root.network.ssid, "testnet", sizeof(root.network.ssid) - 1);
    strncpy(root.network.bssid, "aa:bb:cc:dd:ee:ff", sizeof(root.network.bssid) - 1);
    strncpy(root.network.ip, "192.168.1.50", sizeof(root.network.ip) - 1);
    root.network.connected = true;
    root.network.mdns = (bb_serialize_str_n_t){ .ptr = "bb-test", .len = 7 };

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_health_compose_and_stream(req, &root);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("application/json", cap.content_type);
    TEST_ASSERT_EQUAL_STRING(
        "{\"ok\":true,\"network\":{\"ssid\":\"testnet\","
        "\"bssid\":\"aa:bb:cc:dd:ee:ff\",\"ip\":\"192.168.1.50\","
        "\"connected\":true,\"mdns\":\"bb-test\"},"
        "\"temp\":{\"present\":true,\"soc_c\":55.3},"
        "\"mqtt\":{\"enabled\":true,\"connected\":true}}",
        cap.body);

    bb_http_host_capture_free(&cap);
    bb_mqtt_client_default_set(NULL);
    bb_mqtt_client_destroy(h);
}

// ---------------------------------------------------------------------------
// GET /api/wifi (bb_wifi_http_info_wire_t) -- byte-fidelity golden for the
// B1-1057 migration off bb_wifi_emit_section (the retired bb_json_t
// emitter). Exercises the real production fill fn
// (bb_wifi_http_info_wire_fill) against a hand-built bb_wifi_info_t, so a
// field reorder/type change/present-gating regression in either the
// descriptor OR the fill fn fails here. restart_sta_count/disconnect_rssi
// come from bb_wifi's global test hooks (bb_wifi_test.h,
// BB_WIFI_TESTING) -- same sources the fill fn itself reads via
// bb_wifi_get_restart_sta_count()/bb_wifi_get_disconnect_rssi().
// ---------------------------------------------------------------------------

void test_v2_golden_wifi_info_populated(void)
{
    bb_wifi_test_set_restart_sta_count(3);
    bb_wifi_test_set_disconnect_rssi(-70);

    bb_wifi_info_t info;
    memset(&info, 0, sizeof(info));
    strncpy(info.ssid, "testnet", sizeof(info.ssid) - 1);
    info.bssid[0] = 0xaa; info.bssid[1] = 0xbb; info.bssid[2] = 0xcc;
    info.bssid[3] = 0xdd; info.bssid[4] = 0xee; info.bssid[5] = 0xff;
    info.rssi = -42;
    strncpy(info.ip, "192.168.1.50", sizeof(info.ip) - 1);
    info.connected = true;
    info.disc_reason = BB_WIFI_DISC_UNKNOWN;
    info.disc_age_s = 0;
    info.retry_count = 0;

    bb_wifi_http_info_wire_t snap;
    bb_wifi_http_info_wire_fill(&snap, &info);

    render_eq(&bb_wifi_http_info_wire_desc, &snap,
              "{\"ssid\":\"testnet\",\"bssid\":\"aa:bb:cc:dd:ee:ff\",\"rssi\":-42,"
              "\"ip\":\"192.168.1.50\",\"connected\":true,\"disc_reason\":\"unknown\","
              "\"disc_age_s\":0,\"retry_count\":0,\"restart_sta_count\":3,"
              "\"disconnect_rssi\":-70}");

    bb_wifi_test_set_restart_sta_count(0);
    bb_wifi_test_set_disconnect_rssi(INT8_MIN);
}

void test_v2_golden_wifi_info_disconnected(void)
{
    bb_wifi_test_set_restart_sta_count(0);
    bb_wifi_test_set_disconnect_rssi(0);

    bb_wifi_info_t info;
    memset(&info, 0, sizeof(info));
    strncpy(info.ip, "0.0.0.0", sizeof(info.ip) - 1);
    info.disc_reason = BB_WIFI_DISC_HANDSHAKE_TIMEOUT;
    info.disc_age_s = 42;
    info.retry_count = 5;

    bb_wifi_http_info_wire_t snap;
    bb_wifi_http_info_wire_fill(&snap, &info);

    render_eq(&bb_wifi_http_info_wire_desc, &snap,
              "{\"ssid\":\"\",\"bssid\":\"00:00:00:00:00:00\",\"rssi\":0,"
              "\"ip\":\"0.0.0.0\",\"connected\":false,\"disc_reason\":\"handshake_timeout\","
              "\"disc_age_s\":42,\"retry_count\":5,\"restart_sta_count\":0,"
              "\"disconnect_rssi\":0}");

    bb_wifi_test_set_disconnect_rssi(INT8_MIN);
}

// test_v2_golden — byte-fidelity harness for the v2 wire descriptors
// (B1-767 PR-6): a synthetic bb_serialize worked-example fixture, health
// root-identity slice.
//
// widget fixture: byte-fidelity coverage for a purely synthetic descriptor
// (test_serialize_fixture.h) -- not a spec for any production payload, just
// exercise of the string/bool/i64/str_n/arr_str field-type diversity.
//
// health: each golden string is the SPEC for the ROOT-IDENTITY FIELD SLICE
// ONLY -- the fields health_handler() emits INLINE, BEFORE
// bb_response_build_get() appends the dynamically-registered sections
// ("mqtt"/"temp"). It is NOT the full /api/health document -- see
// bb_health_wire_priv.h for the full slice-vs-document contract. A mismatch
// means the DESCRIPTOR (order/type/max_len/present) is wrong for that
// slice -- fix the descriptor, never the golden.
//
// The info-telemetry envelope golden (which mirrored
// platform/host/bb_pub_info/bb_pub_info.c's info_serialize()) was removed
// with the bb_pub/bb_sink_* cluster cut (B1-905) -- bb_pub_info no longer
// exists, so there is no live path left to spec or diff against.

#include "unity.h"

#include "bb_serialize_json.h"

#include "test_serialize_fixture.h"
#include "../../components/bb_health/bb_health_wire_priv.h"

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
    snap.validated = true;
    strncpy(snap.network.ssid, "testnet", sizeof(snap.network.ssid) - 1);
    strncpy(snap.network.bssid, "aa:bb:cc:dd:ee:ff", sizeof(snap.network.bssid) - 1);
    strncpy(snap.network.ip, "192.168.1.50", sizeof(snap.network.ip) - 1);
    snap.network.connected = true;
    snap.network.mdns = (bb_serialize_str_n_t){ .ptr = "bb-test", .len = 7 };

    render_eq(&bb_health_wire_desc, &snap,
              "{\"ok\":true,\"validated\":true,\"network\":{\"ssid\":\"testnet\","
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
              "{\"ok\":false,\"validated\":false,\"network\":{\"ssid\":\"\","
              "\"bssid\":\"00:00:00:00:00:00\",\"ip\":\"0.0.0.0\","
              "\"connected\":false,\"mdns\":null}}");
}

// ---------------------------------------------------------------------------
// health ROOT-SLICE differential -- documented as NOT tractable here
//
// A true differential for the health root-identity slice (comparing against
// health_handler() BEFORE bb_response_build_get() appends sections) would
// need to invoke that handler directly. It lives in
// platform/espidf/bb_health/bb_health.c -- an ESP-IDF httpd request handler
// (esp_http_server, httpd_req_t) that is not compiled into the native/host
// test target (platformio.ini's native env builds platform/host/* only, per
// test_filter=test_host) and has no host-callable "root fields only, empty
// section registry" entry point exposed today. Isolating the root-field
// emit from the section registry would require restructuring the production
// handler (e.g. splitting a root-only helper out of health_handler()) purely
// to make it host-testable -- out of scope here, which must not contort
// production code for testability. The health-slice goldens above
// (test_v2_golden_health_root_slice_*) remain the coverage for that
// descriptor until a future cutover PR, which will wire the descriptor
// directly into the handler and can then assert true differential/
// regression parity in place.

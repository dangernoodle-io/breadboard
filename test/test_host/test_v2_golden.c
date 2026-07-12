// test_v2_golden — byte-fidelity harness for the v2 wire descriptors
// (B1-767 PR-6): info root-identity slice, health root-identity slice,
// info-telemetry envelope.
//
// info/health: each golden string is the SPEC for the ROOT-IDENTITY FIELD
// SLICE ONLY -- the fields info_handler()/health_handler() emit INLINE,
// BEFORE bb_response_build_get() appends the dynamically-registered
// sections ("build" for info; "mqtt"/"temp" for health). It is NOT the full
// /api/info or /api/health document -- see bb_info_wire_priv.h /
// bb_health_wire_priv.h for the full slice-vs-document contract. A mismatch
// means the DESCRIPTOR (order/type/max_len/present) is wrong for that
// slice -- fix the descriptor, never the golden.
//
// info-telemetry envelope: the envelope IS the whole payload (no section
// registry involved), so this golden genuinely covers the full on-wire
// document produced by platform/host/bb_pub_info/bb_pub_info.c's
// info_serialize() via platform/espidf/bb_cache/bb_cache_espidf.c's
// ts_ms/data envelope wrap. test_v2_golden_info_telem_differential_matches_live_cache
// below additionally proves this against the LIVE cJSON path (not just a
// hand-authored golden) by running both serializers over the same host
// stub state and asserting byte-equality.

#include "unity.h"

#include "bb_serialize_json.h"

#include "../../components/bb_info/bb_info_wire_priv.h"
#include "../../components/bb_health/bb_health_wire_priv.h"
#include "../../components/bb_pub_info/bb_pub_info_wire_priv.h"

#include "bb_pub.h"
#include "bb_pub_info.h"
#include "bb_cache.h"
#include "bb_mem.h"
#include "test_hostname_seed.h"
#include "../../platform/host/bb_board/bb_board_test.h"

#include <string.h>
#include <stdio.h>

// Forward decl, mirroring the same pattern used by
// test_bb_pub_telemetry_fidelity.c / test_bb_cache_evict.c (BB_CACHE_TESTING).
void bb_cache_reset_for_test(void);
void bb_cache_test_set_clock(uint64_t (*fn)(void));

static void render_eq(const bb_serialize_desc_t *d, const void *snap, const char *golden)
{
    char   buf[512];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_serialize_json_render(d, snap, buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL_STRING(golden, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(golden), n);
}

// ---------------------------------------------------------------------------
// info (root-identity slice)
// ---------------------------------------------------------------------------

void test_v2_golden_info_root_slice_populated(void)
{
    bb_info_wire_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.mac, "aa:bb:cc:dd:ee:ff", sizeof(snap.mac) - 1);
    snap.ota_validated = true;
    snap.time_valid    = true;
    snap.boot_epoch_s  = 1704067200;
    strncpy(snap.time_source, "sntp", sizeof(snap.time_source) - 1);
    snap.hostname = (bb_serialize_str_n_t){ .ptr = "bb-test", .len = 7 };

    const char *caps[] = { "display", "led" };
    snap.capabilities = (bb_serialize_arr_str_t){ .items = caps, .count = 2 };

    render_eq(&bb_info_wire_desc, &snap,
              "{\"mac\":\"aa:bb:cc:dd:ee:ff\",\"ota_validated\":true,\"time_valid\":true,"
              "\"boot_epoch_s\":1704067200,\"time_source\":\"sntp\",\"hostname\":\"bb-test\","
              "\"capabilities\":[\"display\",\"led\"]}");
}

void test_v2_golden_info_root_slice_null(void)
{
    bb_info_wire_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.time_source, "none", sizeof(snap.time_source) - 1);
    snap.hostname     = (bb_serialize_str_n_t){ .ptr = NULL, .len = 0 };
    snap.capabilities = (bb_serialize_arr_str_t){ .items = NULL, .count = 0 };

    render_eq(&bb_info_wire_desc, &snap,
              "{\"mac\":\"\",\"ota_validated\":false,\"time_valid\":false,"
              "\"boot_epoch_s\":0,\"time_source\":\"none\",\"hostname\":null,"
              "\"capabilities\":[]}");
}

// ---------------------------------------------------------------------------
// info (root-identity slice) -- boundary / insurance goldens
// ---------------------------------------------------------------------------

// Additional insurance golden: array-walk loop coverage.
void test_v2_golden_info_root_slice_capabilities_three_entries(void)
{
    bb_info_wire_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.time_source, "none", sizeof(snap.time_source) - 1);
    snap.hostname = (bb_serialize_str_n_t){ .ptr = NULL, .len = 0 };

    const char *caps[] = { "display", "led", "mqtt" };
    snap.capabilities = (bb_serialize_arr_str_t){ .items = caps, .count = 3 };

    render_eq(&bb_info_wire_desc, &snap,
              "{\"mac\":\"\",\"ota_validated\":false,\"time_valid\":false,"
              "\"boot_epoch_s\":0,\"time_source\":\"none\",\"hostname\":null,"
              "\"capabilities\":[\"display\",\"led\",\"mqtt\"]}");
}

// (b) mac[18] filled to its exact max_len with NO NUL terminator -- exercises
// the BB_TYPE_STR strnlen(s, max_len) cap: the walker must yield exactly 18
// bytes, never read past the array end looking for a NUL that isn't there.
void test_v2_golden_info_root_slice_mac_boundary_no_nul(void)
{
    bb_info_wire_t snap;
    memset(&snap, 0, sizeof(snap));
    // Exactly 18 bytes, no NUL -- fills bb_info_wire_t.mac (char[18]) to capacity.
    memcpy(snap.mac, "112233445566778899", sizeof(snap.mac));
    strncpy(snap.time_source, "none", sizeof(snap.time_source) - 1);
    snap.hostname     = (bb_serialize_str_n_t){ .ptr = NULL, .len = 0 };
    snap.capabilities = (bb_serialize_arr_str_t){ .items = NULL, .count = 0 };

    render_eq(&bb_info_wire_desc, &snap,
              "{\"mac\":\"112233445566778899\",\"ota_validated\":false,\"time_valid\":false,"
              "\"boot_epoch_s\":0,\"time_source\":\"none\",\"hostname\":null,"
              "\"capabilities\":[]}");
}

// Additional insurance golden: unbounded STR_N path coverage.
void test_v2_golden_info_root_slice_hostname_longer_str_n(void)
{
    bb_info_wire_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.time_source, "none", sizeof(snap.time_source) - 1);
    static const char hostname[] = "breadboard-device-host-01";
    snap.hostname     = (bb_serialize_str_n_t){ .ptr = hostname, .len = sizeof(hostname) - 1 };
    snap.capabilities = (bb_serialize_arr_str_t){ .items = NULL, .count = 0 };

    render_eq(&bb_info_wire_desc, &snap,
              "{\"mac\":\"\",\"ota_validated\":false,\"time_valid\":false,"
              "\"boot_epoch_s\":0,\"time_source\":\"none\","
              "\"hostname\":\"breadboard-device-host-01\",\"capabilities\":[]}");
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
// info-telemetry envelope
// ---------------------------------------------------------------------------

void test_v2_golden_info_telem_psram(void)
{
    bb_info_telem_env_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.ts_ms                          = 1704067200000LL;
    snap.data.heap_internal_free        = 102400;
    snap.data.heap_internal_total       = 204800;
    snap.data.heap_internal_largest_block = 51200;
    snap.data.heap_internal_min_free    = 81920;
    snap.data.has_psram                 = true;
    snap.data.psram_free                = 1048576;
    snap.data.psram_total               = 2097152;
    snap.data.wdt_resets                = 0;
    snap.data.ota_validated             = true;
    snap.data.time_valid                = true;
    snap.data.bb_mem_out                = 0;
    snap.data.bb_mem_peak               = 0;
    snap.data.bb_mem_fail               = 0;

    render_eq(&bb_info_telem_wire_desc, &snap,
              "{\"ts_ms\":1704067200000,\"data\":{\"heap_internal_free\":102400,"
              "\"heap_internal_total\":204800,\"heap_internal_largest_block\":51200,"
              "\"heap_internal_min_free\":81920,\"psram_free\":1048576,"
              "\"psram_total\":2097152,\"wdt_resets\":0,\"ota_validated\":true,"
              "\"time_valid\":true,\"bb_mem_out\":0,\"bb_mem_peak\":0,\"bb_mem_fail\":0}}");
}

void test_v2_golden_info_telem_no_psram(void)
{
    bb_info_telem_env_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.data.has_psram = false;

    render_eq(&bb_info_telem_wire_desc, &snap,
              "{\"ts_ms\":0,\"data\":{\"heap_internal_free\":0,"
              "\"heap_internal_total\":0,\"heap_internal_largest_block\":0,"
              "\"heap_internal_min_free\":0,\"wdt_resets\":0,\"ota_validated\":false,"
              "\"time_valid\":false,\"bb_mem_out\":0,\"bb_mem_peak\":0,\"bb_mem_fail\":0}}");
}

// ---------------------------------------------------------------------------
// info-telemetry envelope -- DIFFERENTIAL parity against the LIVE cJSON path
//
// The goldens above are hand-authored to match the v2 descriptor; they don't
// PROVE parity with today's production serializer. This test runs BOTH
// serializers over the same underlying host state and asserts byte-equality:
//   A. the live path -- bb_pub_info_register() + bb_pub_tick_once() (which
//      drives info_gather()/info_serialize() in
//      platform/host/bb_pub_info/bb_pub_info.c) + bb_cache_get_serialized(),
//      the exact call REST reads go through (bb_cache_espidf.c's
//      {"ts_ms":N,"data":{...}} envelope wrap).
//   B. the v2 descriptor path -- bb_serialize_json_render() against
//      bb_info_telem_wire_desc, fed by a bb_info_telem_env_t built to mirror
//      the same (deterministic, zero/false) host-stub state info_gather()
//      itself reads (bb_meminfo_host.c, bb_diag_panic.c, bb_ntp_host.c,
//      bb_mem's host arena -- all fixed zero/false with no prior activity on
//      a freshly-reset host build).
// ts_ms is pinned via bb_cache_test_set_clock() (BB_CACHE_TESTING) so both
// paths stamp the identical envelope timestamp -- otherwise two real-clock
// reads could legitimately differ by the scheduling gap between them.
static uint64_t fixed_test_clock_ms(void) { return 1704067200000ULL; }

// info is registered BB_PUB_TELEM_SINKS-only (no SSE) -- bb_pub_tick_once()
// skips gather/update entirely when no sink is registered (see
// test_bb_pub_telemetry_fidelity.c's "no_gather_when_tick_not_fired"), so a
// sink MUST be present or this test would silently compare two untouched,
// still-zeroed cache entries rather than a real live-path sample.
static bb_err_t noop_sink_publish(void *ctx, const char *topic,
                                   const char *payload, int len, bool retain)
{
    (void)ctx; (void)topic; (void)payload; (void)len; (void)retain;
    return BB_OK;
}

void test_v2_golden_info_telem_differential_matches_live_cache(void)
{
    bb_pub_test_reset();
    bb_cache_reset_for_test();
    bb_board_test_set_ota_validated(false);
    bb_cache_test_set_clock(fixed_test_clock_ms);

    bb_pub_sink_t sink = { .publish = noop_sink_publish, .ctx = NULL };
    bb_pub_set_sink(&sink);

    TEST_ASSERT_EQUAL(BB_OK, bb_pub_info_register());
    bb_pub_tick_once();

    // A. Live path -- exactly what bb_cache_get_serialized() (the REST read
    // path) hands back: {"ts_ms":N,"data":{...cJSON-serialized info...}}.
    char   live[512];
    size_t live_len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("info", live, sizeof live, &live_len));

    // B. v2 descriptor path -- same logical values. heap/wdt/ota/time fields
    // are deterministic zero/false on a freshly-reset host build (the same
    // host stubs info_gather() itself reads: bb_meminfo_host.c,
    // bb_diag_panic.c, bb_ntp_host.c). bb_mem_out/bb_mem_peak are NOT
    // deterministic zero -- bb_pub/bb_cache's own allocations (e.g. the
    // owned snapshot buffer) leave outstanding bytes on the bb_mem facade by
    // the time gather() samples it -- so read them via the SAME public
    // bb_mem_get_stats() call gather() uses, immediately after the tick
    // (single-threaded test, no allocation churn in between -- the value is
    // stable across the two reads).
    bb_info_telem_env_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.ts_ms           = (int64_t)fixed_test_clock_ms();
    snap.data.has_psram  = false;

    bb_mem_stats_t ms;
    bb_mem_get_stats(&ms);
    snap.data.bb_mem_out  = ms.outstanding_bytes;
    snap.data.bb_mem_peak = ms.peak_outstanding;
    snap.data.bb_mem_fail = ms.alloc_fail;

    char   desc[512];
    size_t desc_len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_json_render(&bb_info_telem_wire_desc, &snap, desc, sizeof desc, &desc_len));

    TEST_ASSERT_EQUAL_STRING(desc, live);

    bb_cache_test_set_clock(NULL);  // restore the real clock for subsequent tests
}

// ---------------------------------------------------------------------------
// info/health ROOT-SLICE differential -- documented as NOT tractable here
//
// A true differential for the info/health root-identity slice (comparing
// against info_handler()/health_handler() BEFORE bb_response_build_get()
// appends sections) would need to invoke those handlers directly. Both live
// in platform/espidf/bb_info/bb_info.c and platform/espidf/bb_health/bb_health.c
// -- ESP-IDF httpd request handlers (esp_http_server, httpd_req_t) that are
// not compiled into the native/host test target (platformio.ini's native env
// builds platform/host/* only, per test_filter=test_host) and have no
// host-callable "root fields only, empty section registry" entry point
// exposed today. Isolating the root-field emit from the section registry
// would require restructuring the production handler (e.g. splitting a
// root-only helper out of info_handler()/health_handler()) purely to make it
// host-testable -- out of scope for this additive PR, which must not
// contort production code for testability. The reworded root-slice goldens
// above (test_v2_golden_info_root_slice_* / test_v2_golden_health_root_slice_*)
// remain the coverage for those two descriptors until the cutover PR, which
// will wire the descriptor directly into the handler and can then assert
// true differential/regression parity in place.

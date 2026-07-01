// bb_pub_wifi — telemetry source satellite: full wifi connection info.
// Compiled on both host (tests) and ESP-IDF.
//
// Migration (telemetry-ssot): uses bb_pub_register_telemetry so the snapshot
// is gathered into bb_cache once per tick; SSE, sinks, and REST all read the
// SAME memoized serialization via bb_cache_get_serialized.  The sample-time
// timestamp (ts_ms) is stamped into the snapshot at gather time and emitted by
// the serializer, so REST == SSE == sink bytes are byte-for-byte identical.
#include "bb_pub_wifi.h"
#include "bb_pub.h"
#include "bb_wifi.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_clock.h"
#include "bb_openapi.h"
#include "bb_registry.h"
#include "../bb_wifi/wifi_hist_priv.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#ifndef CONFIG_BB_PUB_WIFI_AUTO_ATTACH
#define CONFIG_BB_PUB_WIFI_AUTO_ATTACH 0
#endif

// Kconfig host fallback — matches the no-PSRAM Kconfig default.
// On ESP-IDF the build system supplies the real CONFIG_ value via sdkconfig.h.
#ifndef CONFIG_BB_PUB_TELEM_SNAP_MAX
#define CONFIG_BB_PUB_TELEM_SNAP_MAX 512
#endif

static const char *TAG = "bb_pub_wifi";

// ---------------------------------------------------------------------------
// Snapshot struct — captured once per tick under the tick lock.
// Must fit within CONFIG_BB_PUB_TELEM_SNAP_MAX.
// ---------------------------------------------------------------------------

typedef struct {
    bb_wifi_info_t info;             // ~62 bytes
    int            no_ip_count;
    int            egress_dead_count;
    int            lost_ip_count;
    int64_t        ts_ms;            // sample-time monotonic ms (bb_clock_now_ms64)
    // B1-411: new recovery-telemetry fields
    uint32_t       restart_sta_count;
    int8_t         disconnect_rssi;
    // Compact reason-histogram (sentinel buckets + top standard reason).
    uint16_t       hist_lost_ip;       // bucket 99
    uint16_t       hist_egress_dead;   // bucket 100
    uint16_t       hist_no_ip_watchdog;// bucket 101
    uint16_t       hist_top_code;      // highest non-zero standard reason code
    uint16_t       hist_top_count;     // count for hist_top_code
} bb_wifi_snap_t;

// Compile-time guard: wifi snap must fit in the scratch buffer (B1-434).
typedef char _wifi_snap_size_check[
    sizeof(bb_wifi_snap_t) <= CONFIG_BB_PUB_TELEM_SNAP_MAX ? 1 : -1];

// ---------------------------------------------------------------------------
// Host test stub state (only when BB_PUB_WIFI_TESTING is defined)
// ---------------------------------------------------------------------------

#ifdef BB_PUB_WIFI_TESTING
static bb_pub_wifi_test_info_t s_test_info;

void bb_pub_wifi_test_set_info(const bb_pub_wifi_test_info_t *info)
{
    if (!info) {
        memset(&s_test_info, 0, sizeof(s_test_info));
        return;
    }
    s_test_info = *info;
}

void bb_pub_wifi_test_set_rssi(bool connected, int8_t rssi)
{
    memset(&s_test_info, 0, sizeof(s_test_info));
    s_test_info.connected = connected;
    s_test_info.rssi      = rssi;
}
#endif /* BB_PUB_WIFI_TESTING */

// ---------------------------------------------------------------------------
// Gather — fills snap from live wifi state (or test stub); called under lock.
// Returns false to skip this tick (e.g. WiFi not connected).
// ---------------------------------------------------------------------------

// Compute the compact histogram view into a snap from a 256-entry histogram.
static void gather_histogram(bb_wifi_snap_t *snap)
{
    uint16_t hist[256];
    bb_wifi_get_reason_histogram(hist, 256);
    snap->hist_lost_ip        = hist[99];
    snap->hist_egress_dead    = hist[100];
    snap->hist_no_ip_watchdog = hist[101];
    snap->hist_top_code       = wifi_hist_top_reason(hist, &snap->hist_top_count);
}

static bool wifi_gather(void *snap_buf, void *ctx)
{
    (void)ctx;
    bb_wifi_snap_t *snap = snap_buf;

#ifdef BB_PUB_WIFI_TESTING
    if (!s_test_info.connected) return false;

    memset(snap, 0, sizeof(*snap));
    snap->info.connected   = s_test_info.connected;
    snap->info.rssi        = s_test_info.rssi;
    strncpy(snap->info.ssid, s_test_info.ssid, sizeof(snap->info.ssid) - 1);
    memcpy(snap->info.bssid, s_test_info.bssid, sizeof(snap->info.bssid));
    strncpy(snap->info.ip, s_test_info.ip, sizeof(snap->info.ip) - 1);
    snap->info.disc_reason  = s_test_info.disc_reason;
    snap->info.disc_age_s   = s_test_info.disc_age_s;
    snap->info.retry_count  = s_test_info.retry_count;
    snap->no_ip_count       = (int)bb_wifi_get_no_ip_count();
    snap->egress_dead_count = (int)bb_wifi_get_egress_dead_count();
    snap->lost_ip_count     = (int)bb_wifi_get_lost_ip_count();
    snap->ts_ms             = (int64_t)bb_clock_now_ms64();
    snap->restart_sta_count = bb_wifi_get_restart_sta_count();
    snap->disconnect_rssi   = bb_wifi_get_disconnect_rssi();
    gather_histogram(snap);
    return true;
#else
    if (!bb_wifi_has_ip()) return false;

    bb_wifi_info_t info;
    bb_err_t rc = bb_wifi_get_info(&info);
    if (rc != BB_OK) return false;

    memset(snap, 0, sizeof(*snap));
    snap->info              = info;
    snap->no_ip_count       = (int)bb_wifi_get_no_ip_count();
    snap->egress_dead_count = (int)bb_wifi_get_egress_dead_count();
    snap->lost_ip_count     = (int)bb_wifi_get_lost_ip_count();
    snap->ts_ms             = (int64_t)bb_clock_now_ms64();
    snap->restart_sta_count = bb_wifi_get_restart_sta_count();
    snap->disconnect_rssi   = bb_wifi_get_disconnect_rssi();
    gather_histogram(snap);
    return true;
#endif
}

// ---------------------------------------------------------------------------
// Serialize — called by bb_cache (via bb_cache_get_serialized for the push path
// and REST) to build the JSON from the frozen snapshot.  Emits ts_ms from the
// snapshot so the memoized bytes are identical across REST/SSE/sink.
// Mirrors bb_wifi_emit_section but reads fully from snap (SSOT guarantee).
// ---------------------------------------------------------------------------

static void wifi_serialize(bb_json_t obj, const void *snap_raw)
{
    const bb_wifi_snap_t *snap = snap_raw;
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             snap->info.bssid[0], snap->info.bssid[1], snap->info.bssid[2],
             snap->info.bssid[3], snap->info.bssid[4], snap->info.bssid[5]);

    bb_json_obj_set_string(obj, "ssid",             snap->info.ssid);
    bb_json_obj_set_string(obj, "bssid",            bssid);
    bb_json_obj_set_int   (obj, "rssi",             (int64_t)snap->info.rssi);
    bb_json_obj_set_string(obj, "ip",               snap->info.ip);
    bb_json_obj_set_bool  (obj, "connected",        snap->info.connected);
    bb_json_obj_set_int   (obj, "disc_reason",      (int64_t)snap->info.disc_reason);
    bb_json_obj_set_int   (obj, "disc_age_s",       (int64_t)snap->info.disc_age_s);
    bb_json_obj_set_int   (obj, "retry_count",      (int64_t)snap->info.retry_count);
    bb_json_obj_set_int   (obj, "no_ip_recoveries",  (int64_t)snap->no_ip_count);
    bb_json_obj_set_int   (obj, "egress_dead_count", (int64_t)snap->egress_dead_count);
    bb_json_obj_set_int   (obj, "lost_ip_count",     (int64_t)snap->lost_ip_count);
    bb_json_obj_set_int   (obj, "recovery_count",
                           (int64_t)(snap->no_ip_count +
                                     snap->egress_dead_count +
                                     snap->lost_ip_count));
    bb_json_obj_set_int   (obj, "ts_ms",              snap->ts_ms);
    bb_json_obj_set_int   (obj, "restart_sta_count",  (int64_t)snap->restart_sta_count);
    bb_json_obj_set_int   (obj, "disconnect_rssi",    (int64_t)snap->disconnect_rssi);

    bb_json_t h = bb_json_obj_new();
    bb_json_obj_set_int(h, "lost_ip",          (int64_t)snap->hist_lost_ip);
    bb_json_obj_set_int(h, "egress_dead",       (int64_t)snap->hist_egress_dead);
    bb_json_obj_set_int(h, "no_ip_watchdog",    (int64_t)snap->hist_no_ip_watchdog);
    bb_json_obj_set_int(h, "top_reason_code",   (int64_t)snap->hist_top_code);
    bb_json_obj_set_int(h, "top_reason_count",  (int64_t)snap->hist_top_count);
    bb_json_obj_set_obj(obj, "reason_histogram", h);
}

// ---------------------------------------------------------------------------
// Schema + Registration
// ---------------------------------------------------------------------------

static const char k_wifi_telemetry_schema[] =
    "{\"title\":\"WifiTelemetry\",\"x-sse-topic\":\"wifi\",\"type\":\"object\","
    "\"properties\":{"
    "\"ssid\":{\"type\":\"string\"},"
    "\"bssid\":{\"type\":\"string\"},"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"ip\":{\"type\":\"string\"},"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"disc_reason\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"retry_count\":{\"type\":\"integer\"},"
    "\"no_ip_recoveries\":{\"type\":\"integer\"},"
    "\"egress_dead_count\":{\"type\":\"integer\"},"
    "\"lost_ip_count\":{\"type\":\"integer\"},"
    "\"recovery_count\":{\"type\":\"integer\"},"
    "\"ts_ms\":{\"type\":\"integer\"},"
    "\"restart_sta_count\":{\"type\":\"integer\"},"
    "\"disconnect_rssi\":{\"type\":\"integer\"},"
    "\"reason_histogram\":{\"type\":\"object\","
      "\"properties\":{"
      "\"lost_ip\":{\"type\":\"integer\"},"
      "\"egress_dead\":{\"type\":\"integer\"},"
      "\"no_ip_watchdog\":{\"type\":\"integer\"},"
      "\"top_reason_code\":{\"type\":\"integer\"},"
      "\"top_reason_count\":{\"type\":\"integer\"}}}},"
    "\"required\":[\"ssid\",\"connected\",\"rssi\",\"ts_ms\"]}";

bb_err_t bb_pub_wifi_register(void)
{
    bb_pub_telemetry_cfg_t cfg = {
        .topic     = "wifi",
        .gather    = wifi_gather,
        .serialize = wifi_serialize,
        .snap_size = sizeof(bb_wifi_snap_t),
        .flags     = BB_PUB_TELEM_SSE | BB_PUB_TELEM_SINKS,
        .ctx       = NULL,
    };

    bb_openapi_register_topic_schema("wifi", k_wifi_telemetry_schema, "WifiTelemetry");

    bb_err_t err = bb_pub_register_telemetry(&cfg);
    if (err == BB_OK) {
        bb_log_i(TAG, "registered wifi telemetry source");
    } else if (err != BB_ERR_NO_SPACE) {
        bb_log_w(TAG, "register_telemetry failed: %d", err);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Auto-attach (PRE_HTTP tier, after bb_pub's own PRE_HTTP registration)
// ---------------------------------------------------------------------------

static bb_err_t bb_pub_wifi_init(void)
{
    return bb_pub_wifi_register();
}

#if CONFIG_BB_PUB_WIFI_AUTO_ATTACH
BB_REGISTRY_REGISTER_PRE_HTTP(bb_pub_wifi, bb_pub_wifi_init);
#endif

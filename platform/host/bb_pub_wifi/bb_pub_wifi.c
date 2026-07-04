// bb_pub_wifi — telemetry source satellite: full wifi connection info.
// Compiled on both host (tests) and ESP-IDF.
//
// Migration (telemetry-ssot): uses bb_pub_register_telemetry so the snapshot
// is gathered into bb_cache once per tick; SSE, sinks, and REST all read the
// SAME memoized serialization via bb_cache_get_serialized.  bb_cache owns the
// envelope's ts_ms (B1-570 PR-3) — this source no longer stamps or emits its
// own timestamp; REST == SSE == sink bytes stay byte-for-byte identical
// because bb_cache freezes ts_ms + the memoized "data" bytes between updates.
//
// B1-486: this is the live producer behind GET /api/wifi's bb_cache entry
// (topic "wifi"). The recovery counters (no_ip_recoveries, egress_dead_count,
// lost_ip_count, recovery_count) and reason_histogram have moved to
// GET /api/diag/net (bb_net_health) — the single source of truth for
// wifi recovery counters. This topic keeps only connection-state fields plus
// restart_sta_count/disconnect_rssi.
//
// roam_count/roam_age_s (B1-497) are likewise consolidated onto the
// /api/diag/net + net.health discriminator surface only — no longer
// duplicated here (net-health SSOT, wifi-netmode PR).
#include "bb_pub_wifi.h"
#include "bb_pub.h"
#include "bb_pub_defaults.h"
#include "bb_wifi.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_openapi.h"
#include "bb_init.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#ifndef CONFIG_BB_PUB_WIFI_AUTO_ATTACH
#define CONFIG_BB_PUB_WIFI_AUTO_ATTACH 0
#endif

static const char *TAG = "bb_pub_wifi";

// ---------------------------------------------------------------------------
// Snapshot struct — captured once per tick under the tick lock.
// Must fit within CONFIG_BB_PUB_TELEM_SNAP_MAX.
// ---------------------------------------------------------------------------

typedef struct {
    bb_wifi_info_t info;             // ~62 bytes
    // B1-411: new recovery-telemetry fields
    uint32_t       restart_sta_count;
    int8_t         disconnect_rssi;
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
    snap->restart_sta_count = bb_wifi_get_restart_sta_count();
    snap->disconnect_rssi   = bb_wifi_get_disconnect_rssi();
    return true;
#else
    if (!bb_wifi_has_ip()) return false;

    bb_wifi_info_t info;
    bb_err_t rc = bb_wifi_get_info(&info);
    if (rc != BB_OK) return false;

    memset(snap, 0, sizeof(*snap));
    snap->info              = info;
    snap->restart_sta_count = bb_wifi_get_restart_sta_count();
    snap->disconnect_rssi   = bb_wifi_get_disconnect_rssi();
    return true;
#endif
}

// ---------------------------------------------------------------------------
// Serialize — called by bb_cache (via bb_cache_get_serialized for the push path
// and REST) to build the JSON from the frozen snapshot.  ts_ms is applied by
// bb_cache's envelope wrap, not emitted here (B1-570 PR-3).
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
    bb_json_obj_set_int   (obj, "restart_sta_count",  (int64_t)snap->restart_sta_count);
    bb_json_obj_set_int   (obj, "disconnect_rssi",    (int64_t)snap->disconnect_rssi);
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
    "\"restart_sta_count\":{\"type\":\"integer\"},"
    "\"disconnect_rssi\":{\"type\":\"integer\"}},"
    "\"required\":[\"ssid\",\"connected\",\"rssi\"]}";

bb_err_t bb_pub_wifi_register(void)
{
    bb_pub_telemetry_cfg_t cfg = {
        .topic     = BB_TOPIC_WIFI,
        .gather    = wifi_gather,
        .serialize = wifi_serialize,
        .snap_size = sizeof(bb_wifi_snap_t),
        .flags     = BB_PUB_TELEM_SSE | BB_PUB_TELEM_SINKS,
        .ctx       = NULL,
    };

    bb_openapi_register_topic_schema(BB_TOPIC_WIFI, k_wifi_telemetry_schema, "WifiTelemetry");

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
BB_INIT_REGISTER_PRE_HTTP(bb_pub_wifi, bb_pub_wifi_init);
#endif

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
#include "bb_registry.h"
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
// Must fit within CONFIG_BB_PUB_TELEM_SNAP_MAX (default 256 bytes).
// ---------------------------------------------------------------------------

typedef struct {
    bb_wifi_info_t info;     // ~62 bytes
    int            no_ip_count;
    int64_t        ts_ms;    // sample-time monotonic ms (bb_clock_now_ms64)
} bb_wifi_snap_t;

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
    snap->info.disc_reason = s_test_info.disc_reason;
    snap->info.disc_age_s  = s_test_info.disc_age_s;
    snap->info.retry_count = s_test_info.retry_count;
    snap->no_ip_count      = (int)bb_wifi_get_no_ip_count();
    snap->ts_ms            = (int64_t)bb_clock_now_ms64();
    return true;
#else
    if (!bb_wifi_has_ip()) return false;

    bb_wifi_info_t info;
    bb_err_t rc = bb_wifi_get_info(&info);
    if (rc != BB_OK) return false;

    memset(snap, 0, sizeof(*snap));
    snap->info        = info;
    snap->no_ip_count = (int)bb_wifi_get_no_ip_count();
    snap->ts_ms       = (int64_t)bb_clock_now_ms64();
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
    bb_json_obj_set_int   (obj, "no_ip_recoveries", (int64_t)snap->no_ip_count);
    bb_json_obj_set_int   (obj, "ts_ms",            snap->ts_ms);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

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

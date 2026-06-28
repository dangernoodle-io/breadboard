// bb_pub_power — telemetry source satellite: power readings.
// Compiled on both host (tests) and ESP-IDF.
//
// Migration (telemetry-ssot): uses bb_pub_register_telemetry so the snapshot
// is gathered into bb_cache once per tick; SSE, sinks, and REST all read the
// SAME memoized serialization.  ts_ms is stamped at gather time.
#include "bb_pub_power.h"
#include "bb_pub.h"
#include "bb_power.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_clock.h"
#include "bb_registry.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef CONFIG_BB_PUB_POWER_AUTO_ATTACH
#define CONFIG_BB_PUB_POWER_AUTO_ATTACH 0
#endif

static const char *TAG = "bb_pub_power";

// ---------------------------------------------------------------------------
// Snapshot struct — ~28 bytes, well under 256-byte limit.
// ---------------------------------------------------------------------------

typedef struct {
    bb_power_snapshot_t snap;  // 5 ints = 20 bytes
    int64_t             ts_ms;
} bb_power_snap_t;

// ---------------------------------------------------------------------------
// Gather
// ---------------------------------------------------------------------------

static bool power_gather(void *snap_buf, void *ctx)
{
    (void)ctx;

    bb_power_handle_t h = bb_power_primary();
    if (!h) return false;

    bb_power_snap_t *s = snap_buf;
    memset(s, 0, sizeof(*s));
    bb_power_snapshot(h, &s->snap);
    s->ts_ms = (int64_t)bb_clock_now_ms64();
    return true;
}

// ---------------------------------------------------------------------------
// Serialize — emits fields from frozen snapshot.  Mirrors bb_power_emit.
// ---------------------------------------------------------------------------

static void power_serialize(bb_json_t obj, const void *snap_raw)
{
    const bb_power_snap_t *s = snap_raw;
    bb_power_emit(obj, &s->snap);
    bb_json_obj_set_int(obj, "ts_ms", s->ts_ms);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bb_err_t bb_pub_power_register(void)
{
    bb_pub_telemetry_cfg_t cfg = {
        .topic     = "power",
        .gather    = power_gather,
        .serialize = power_serialize,
        .snap_size = sizeof(bb_power_snap_t),
        .flags     = BB_PUB_TELEM_SSE | BB_PUB_TELEM_SINKS,
        .ctx       = NULL,
    };

    bb_err_t err = bb_pub_register_telemetry(&cfg);
    if (err == BB_OK) {
        bb_log_i(TAG, "registered power telemetry source");
    } else if (err != BB_ERR_NO_SPACE) {
        bb_log_w(TAG, "register_telemetry failed: %d", err);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Auto-attach (PRE_HTTP tier, after bb_pub's own PRE_HTTP registration)
// ---------------------------------------------------------------------------

static bb_err_t bb_pub_power_init(void)
{
    return bb_pub_power_register();
}

#if CONFIG_BB_PUB_POWER_AUTO_ATTACH
BB_REGISTRY_REGISTER_PRE_HTTP(bb_pub_power, bb_pub_power_init);
#endif

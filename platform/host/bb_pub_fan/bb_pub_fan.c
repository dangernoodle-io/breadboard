// bb_pub_fan — telemetry source satellite: fan readings.
// Compiled on both host (tests) and ESP-IDF.
//
// Migration (telemetry-ssot): uses bb_pub_register_telemetry so the snapshot
// is gathered into bb_cache once per tick; SSE, sinks, and REST all read the
// SAME memoized serialization.  ts_ms is stamped at gather time.
#include "bb_pub_fan.h"
#include "bb_pub.h"
#include "bb_fan.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_clock.h"
#include "bb_registry.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef CONFIG_BB_PUB_FAN_AUTO_ATTACH
#define CONFIG_BB_PUB_FAN_AUTO_ATTACH 0
#endif

static const char *TAG = "bb_pub_fan";

// ---------------------------------------------------------------------------
// Snapshot struct — captured once per tick under the tick lock.
// Must fit within CONFIG_BB_PUB_TELEM_SNAP_MAX (default 256 bytes).
// Plain: ~24 bytes; with autofan: ~40 bytes.
// ---------------------------------------------------------------------------

typedef struct {
    bb_fan_snapshot_t snap;
#ifdef CONFIG_BB_FAN_AUTOFAN
    float             die_ema_c;
    float             aux_ema_c;
    float             pid_input_c;
    bool              pid_is_vr;  // true="vr", false="die" (avoids pointer in snap)
#endif
    int64_t           ts_ms;
} bb_fan_snap_t;

// ---------------------------------------------------------------------------
// Gather — fills snap from live fan state; called under tick lock.
// ---------------------------------------------------------------------------

static bool fan_gather(void *snap_buf, void *ctx)
{
    (void)ctx;

    bb_fan_handle_t h = bb_fan_primary();
    if (!h) return false;

    bb_fan_snap_t *s = snap_buf;
    memset(s, 0, sizeof(*s));
    bb_fan_snapshot(h, &s->snap);

#ifdef CONFIG_BB_FAN_AUTOFAN
    bb_fan_autofan_telemetry_t tel;
    bb_fan_get_autofan_telemetry(h, &tel);
    s->die_ema_c   = tel.die_ema_c;
    s->aux_ema_c   = tel.aux_ema_c;
    s->pid_input_c = tel.pid_input_c;
    // "aux" maps to "vr" on the wire; internal src strings are "die" or "aux"
    s->pid_is_vr   = (tel.pid_input_src && tel.pid_input_src[0] == 'a');
#endif

    s->ts_ms = (int64_t)bb_clock_now_ms64();
    return true;
}

// ---------------------------------------------------------------------------
// Serialize — emits fields from frozen snapshot.  Mirrors bb_fan_emit.
// ---------------------------------------------------------------------------

static void fan_serialize(bb_json_t obj, const void *snap_raw)
{
    const bb_fan_snap_t *s = snap_raw;

    if (s->snap.rpm >= 0) {
        bb_json_obj_set_int(obj, "rpm", (int64_t)s->snap.rpm);
    } else {
        bb_json_obj_set_null(obj, "rpm");
    }
    if (s->snap.duty_pct >= 0) {
        bb_json_obj_set_int(obj, "duty_pct", (int64_t)s->snap.duty_pct);
    } else {
        bb_json_obj_set_null(obj, "duty_pct");
    }
    if (!isnan(s->snap.die_c)) {
        bb_json_obj_set_number(obj, "die_c", (double)s->snap.die_c);
    } else {
        bb_json_obj_set_null(obj, "die_c");
    }
    if (!isnan(s->snap.board_c)) {
        bb_json_obj_set_number(obj, "board_c", (double)s->snap.board_c);
    } else {
        bb_json_obj_set_null(obj, "board_c");
    }

#ifdef CONFIG_BB_FAN_AUTOFAN
    if (s->die_ema_c >= 0.0f) {
        bb_json_obj_set_number(obj, "die_ema_c", (double)s->die_ema_c);
    } else {
        bb_json_obj_set_null(obj, "die_ema_c");
    }
    if (s->aux_ema_c >= 0.0f) {
        bb_json_obj_set_number(obj, "vr_ema_c", (double)s->aux_ema_c);
    } else {
        bb_json_obj_set_null(obj, "vr_ema_c");
    }
    if (s->pid_input_c >= 0.0f) {
        bb_json_obj_set_number(obj, "pid_input_c", (double)s->pid_input_c);
    } else {
        bb_json_obj_set_null(obj, "pid_input_c");
    }
    bb_json_obj_set_string(obj, "pid_input_src", s->pid_is_vr ? "vr" : "die");
#endif

    bb_json_obj_set_int(obj, "ts_ms", s->ts_ms);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bb_err_t bb_pub_fan_register(void)
{
    bb_pub_telemetry_cfg_t cfg = {
        .topic     = "fan",
        .gather    = fan_gather,
        .serialize = fan_serialize,
        .snap_size = sizeof(bb_fan_snap_t),
        .flags     = BB_PUB_TELEM_SSE | BB_PUB_TELEM_SINKS,
        .ctx       = NULL,
    };

    bb_err_t err = bb_pub_register_telemetry(&cfg);
    if (err == BB_OK) {
        bb_log_i(TAG, "registered fan telemetry source");
    } else if (err != BB_ERR_NO_SPACE) {
        bb_log_w(TAG, "register_telemetry failed: %d", err);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Auto-attach (PRE_HTTP tier, after bb_pub's own PRE_HTTP registration)
// ---------------------------------------------------------------------------

static bb_err_t bb_pub_fan_init(void)
{
    return bb_pub_fan_register();
}

#if CONFIG_BB_PUB_FAN_AUTO_ATTACH
BB_REGISTRY_REGISTER_PRE_HTTP(bb_pub_fan, bb_pub_fan_init);
#endif

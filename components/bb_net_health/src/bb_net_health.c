// bb_net_health — pure link-health classifier.
//
// No ESP-IDF dependencies; compiles on host and device.
// See bb_net_health.h for threshold and hysteresis constants.
#include "bb_net_health.h"
#include "bb_json.h"
#include <stddef.h>

// ---------------------------------------------------------------------------
// Heap state (module static; zero-init = BB_HEAP_STATE_OK)
// ---------------------------------------------------------------------------

static bb_heap_state_t s_heap_state = BB_HEAP_STATE_OK;

bb_heap_state_t bb_net_health_classify_heap(size_t free_bytes)
{
    if (free_bytes < (size_t)BB_NET_HEALTH_HEAP_CRITICAL_BYTES) {
        return BB_HEAP_STATE_CRITICAL;
    }
    if (free_bytes < (size_t)BB_NET_HEALTH_HEAP_LOW_BYTES) {
        return BB_HEAP_STATE_LOW;
    }
    return BB_HEAP_STATE_OK;
}

bb_heap_state_t bb_net_health_heap_state(void)
{
    return s_heap_state;
}

// Internal setter — called only from platform/espidf/bb_net_health/bb_net_health_espidf.c.
// Not declared in the public header; espidf.c forward-declares it with extern.
void bb_net_health_set_heap_state(bb_heap_state_t state)
{
    s_heap_state = state;
}

const char *bb_heap_state_str(bb_heap_state_t state)
{
    switch (state) {
    case BB_HEAP_STATE_OK:       return "ok";
    case BB_HEAP_STATE_LOW:      return "low";
    case BB_HEAP_STATE_CRITICAL: return "critical";
    default:                     return "ok";
    }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Classify raw RSSI into a state bucket (no hysteresis).
static bb_net_state_t rssi_bucket(int8_t rssi)
{
    // rssi >= 0 means no valid reading / not associated — treat as POOR.
    if (rssi >= 0) {
        return BB_NET_STATE_POOR;
    }
    if (rssi >= BB_NET_HEALTH_RSSI_GOOD) {
        return BB_NET_STATE_GOOD;
    }
    if (rssi >= BB_NET_HEALTH_RSSI_MARGINAL_LO) {
        return BB_NET_STATE_MARGINAL;
    }
    return BB_NET_STATE_POOR;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void bb_net_health_eval(bb_net_health_state_t       *st,
                        const bb_net_health_input_t *in,
                        bb_net_health_output_t      *out)
{
    bb_net_state_t raw = rssi_bucket(in->rssi);

    // Cold-start seeding: on the very first call, bypass hysteresis and report
    // the raw bucket directly so the boot snapshot reflects reality immediately.
    if (!st->initialized) {
        st->current_state = raw;
        st->initialized   = true;
    } else {
        // Hysteresis: downgrade requires BB_NET_HEALTH_HYST_DOWN consecutive
        // worse-bucket samples; upgrade requires BB_NET_HEALTH_HYST_UP
        // consecutive better-bucket samples.
        if (raw > st->current_state) {
            // Moving to a worse bucket.
            st->down_count++;
            st->up_count = 0;
            if (st->down_count >= BB_NET_HEALTH_HYST_DOWN) {
                st->current_state = raw;
                st->down_count    = 0;
            }
        } else if (raw < st->current_state) {
            // Moving to a better bucket.
            st->up_count++;
            st->down_count = 0;
            if (st->up_count >= BB_NET_HEALTH_HYST_UP) {
                st->current_state = raw;
                st->up_count      = 0;
            }
        } else {
            // Same bucket — clear transition counters.
            st->down_count = 0;
            st->up_count   = 0;
        }
    }

    // Sustained-poor counter (for adaptive backoff in commit 4).
    if (st->current_state == BB_NET_STATE_POOR) {
        st->sustained_poor_count++;
    } else {
        st->sustained_poor_count = 0;
    }

    // Early-warning: true when any of:
    //  1. Classified state is POOR and has been sustained >= HYST_DOWN samples
    //     (i.e. state just became or remains POOR).
    //  2. mqtt_reconnect_count increased since last eval.
    //  3. !mqtt_connected AND mqtt_disc_age_s is in (0, 60) s — a recent MQTT
    //     disconnect has been recorded.  Uses the MQTT-specific disconnect age,
    //     not the WiFi disc_age_s, so a broker refusal with WiFi still up
    //     correctly trips the warning.  mqtt_disc_age_s == 0 means "no
    //     disconnect observed yet" (e.g. fresh boot before first connect) and
    //     does NOT trigger the warning.
    bool warn_poor      = (st->current_state == BB_NET_STATE_POOR);
    bool warn_reconnect = (in->mqtt_reconnect_count > st->last_reconnect_count);
    bool warn_disc      = (!in->mqtt_connected &&
                           in->mqtt_disc_age_s > 0U &&
                           in->mqtt_disc_age_s < 60U);

    out->state         = st->current_state;
    out->early_warning = warn_poor || warn_reconnect || warn_disc;

    // Update last-seen reconnect count for next eval.
    st->last_reconnect_count = in->mqtt_reconnect_count;
}

const char *bb_net_state_str(bb_net_state_t state)
{
    switch (state) {
    case BB_NET_STATE_GOOD:     return "good";
    case BB_NET_STATE_MARGINAL: return "marginal";
    case BB_NET_STATE_POOR:     return "poor";
    default:                    return "poor";
    }
}

bool bb_net_health_throttle_decision(bb_net_health_state_t *st, int threshold)
{
    if (!st->throttled) {
        // Start throttling when POOR is sustained >= threshold samples.
        if (st->sustained_poor_count >= threshold) {
            st->throttled = true;
        }
    } else {
        // Stop throttling when state recovers to GOOD or MARGINAL.
        if (st->current_state != BB_NET_STATE_POOR) {
            st->throttled = false;
        }
    }
    return st->throttled;
}

void bb_net_health_emit_status(bb_json_t obj, const bb_net_health_status_t *snap)
{
    bb_json_obj_set_string(obj, "state",         bb_net_state_str(snap->state));
    bb_json_obj_set_bool  (obj, "early_warning", snap->early_warning);
    bb_json_obj_set_bool  (obj, "throttled",     snap->throttled);

    bb_json_t mqtt = bb_json_obj_new();
    if (mqtt) {
        bb_json_obj_set_bool(mqtt, "connected", snap->mqtt_connected);
        bb_json_obj_set_obj(obj, "mqtt", mqtt);
    }

    bb_json_t http = bb_json_obj_new();
    if (http) {
        bb_json_obj_set_bool(http, "connected", snap->http_connected);
        bb_json_obj_set_obj(obj, "http", http);
    }
}

void bb_net_health_emit(bb_json_t obj, const void *snap_v)
{
    const bb_net_health_status_t *snap = (const bb_net_health_status_t *)snap_v;
    bb_json_obj_set_number(obj, "rssi",                   (double)snap->rssi);
    bb_json_obj_set_string(obj, "state",                  bb_net_state_str(snap->state));
    bb_json_obj_set_bool  (obj, "early_warning",          snap->early_warning);
    bb_json_obj_set_bool  (obj, "throttled",              snap->throttled);
    bb_json_obj_set_number(obj, "last_disconnect_reason", (double)snap->last_disconnect_reason);
    bb_json_obj_set_number(obj, "disc_age_s",             (double)snap->disc_age_s);
    bb_json_obj_set_number(obj, "lost_ip_recoveries",     (double)snap->lost_ip_recoveries);
    bb_json_obj_set_number(obj, "lost_ip_age_s",          (double)snap->lost_ip_age_s);
    bb_json_obj_set_int(obj, "egress_dead_recoveries", (int64_t)snap->egress_dead_recoveries);

    bb_json_t mqtt = bb_json_obj_new();
    if (mqtt) {
        bb_json_obj_set_bool  (mqtt, "connected",       snap->mqtt_connected);
        bb_json_obj_set_number(mqtt, "reconnect_count", (double)snap->mqtt_reconnect_count);
        bb_json_obj_set_number(mqtt, "disc_age_s",      (double)snap->mqtt_disc_age_s);
        bb_json_obj_set_number(mqtt, "disc_reason",     (double)snap->mqtt_disc_reason);
        bb_json_obj_set_number(mqtt, "tls_fail",        (double)snap->mqtt_tls_fail);
        bb_json_obj_set_obj(obj, "mqtt", mqtt);
    }

    bb_json_t http = bb_json_obj_new();
    if (http) {
        bb_json_obj_set_bool  (http, "connected",       snap->http_connected);
        bb_json_obj_set_number(http, "consec_failures", (double)snap->http_consec_failures);
        bb_json_obj_set_number(http, "tls_fail",        (double)snap->http_tls_fail);
        bb_json_obj_set_number(http, "last_status",     (double)snap->http_last_status);
        bb_json_obj_set_obj(obj, "http", http);
    }
}

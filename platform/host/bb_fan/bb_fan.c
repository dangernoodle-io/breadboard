// bb_fan dispatch layer — platform-independent; shared by espidf and host.
// Mutex approach: pthread_mutex_t on host; on ESP-IDF pthread.h is also
// available (ESP-IDF ships a POSIX pthread layer), so a single
// PTHREAD_MUTEX_INITIALIZER works on both targets.
#include "bb_fan.h"
#include "bb_fan_driver.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_lock.h"
#include <stdlib.h>
#include <math.h>
#include <pthread.h>

#ifdef CONFIG_BB_FAN_AUTOFAN
#include "../../../components/bb_fan/src/bb_fan_pid.h"
#endif

static const char *TAG = "bb_fan";

struct bb_fan {
    const bb_fan_driver_t *drv;
    void *state;
    bb_fan_snapshot_t cache;
    pthread_mutex_t lock;
#ifdef CONFIG_BB_FAN_AUTOFAN
    // Autofan config (protected by lock)
    bb_fan_autofan_cfg_t autofan_cfg;
    // EMA state (<0 = uninitialized)
    float die_ema;
    float aux_ema;
    // Last aux temp fed by consumer (<0 = not available)
    float aux_temp_raw;
    // PID state
    bb_fan_pid_t pid;
    float pid_input;
    float pid_output;
    float pid_setpoint;
    bool  pid_armed;
    // Telemetry (protected by lock, written by poll)
    float tel_die_ema;
    float tel_aux_ema;
    float tel_pid_input_c;
    const char *tel_pid_input_src;
#endif
};

static bb_fan_handle_t s_primary = NULL;

bb_err_t bb_fan_handle_create(const bb_fan_driver_t *drv, void *state,
                               bb_fan_handle_t *out)
{
    if (!drv || !out) return BB_ERR_INVALID_ARG;
    bb_fan_handle_t h = (bb_fan_handle_t)calloc(1, sizeof(struct bb_fan));
    if (!h) return BB_ERR_NO_SPACE; // LCOV_EXCL_LINE
    h->drv   = drv;
    h->state = state;
    // All snapshot fields start at unavailable until first poll.
    h->cache.rpm      = -1;
    h->cache.duty_pct = -1;
    h->cache.die_c    = NAN;
    h->cache.board_c  = NAN;
    pthread_mutex_init(&h->lock, NULL);
#ifdef CONFIG_BB_FAN_AUTOFAN
    // Autofan defaults (matching TM config defaults)
    h->autofan_cfg.enabled      = false;
    h->autofan_cfg.die_target_c = 60.0f;
    h->autofan_cfg.aux_target_c = 75.0f;
    h->autofan_cfg.min_pct      = 25;
    h->autofan_cfg.manual_pct   = 100;
    h->die_ema        = -1.0f;
    h->aux_ema        = -1.0f;
    h->aux_temp_raw   = -1.0f;
    h->tel_die_ema        = -1.0f;
    h->tel_aux_ema        = -1.0f;
    h->tel_pid_input_c    = -1.0f;
    h->tel_pid_input_src  = "";
    // PID init: Kp=5 Ki=0.1 Kd=2, P_ON_E, REVERSE, 5000ms sample time, limits [min,100]
    h->pid_input    = h->autofan_cfg.die_target_c;
    h->pid_output   = (float)h->autofan_cfg.min_pct;
    h->pid_setpoint = h->autofan_cfg.die_target_c;
    bb_fan_pid_init(&h->pid, &h->pid_input, &h->pid_output, &h->pid_setpoint,
                    5.0f, 0.1f, 2.0f, BB_FAN_PID_P_ON_E, BB_FAN_PID_REVERSE);
    bb_fan_pid_set_sample_time(&h->pid, 5000);
    bb_fan_pid_set_output_limits(&h->pid, (float)h->autofan_cfg.min_pct, 100.0f);
    h->pid_armed = false;
#endif
    *out = h;
    return BB_OK;
}

bb_err_t bb_fan_poll(bb_fan_handle_t h)
{
    if (!h || !h->drv) return BB_ERR_INVALID_STATE;

    bb_fan_snapshot_t s;

    s.rpm      = h->drv->read_rpm      ? h->drv->read_rpm(h->state)      : -1;
    s.duty_pct = h->drv->get_duty_pct  ? h->drv->get_duty_pct(h->state)  : -1;

    if (h->drv->read_die_temp_c) {
        float t = NAN;
        s.die_c = (h->drv->read_die_temp_c(h->state, &t) == BB_OK) ? t : NAN;
    } else {
        s.die_c = NAN;
    }

    if (h->drv->read_board_temp_c) {
        float t = NAN;
        s.board_c = (h->drv->read_board_temp_c(h->state, &t) == BB_OK) ? t : NAN;
    } else {
        s.board_c = NAN;
    }

#ifdef CONFIG_BB_FAN_AUTOFAN
    // Read a local copy of autofan config under lock, then run PID outside lock
    // (pid_compute only touches h->pid* fields; we serialize with the full lock).
    pthread_mutex_lock(&h->lock);
    bb_fan_autofan_cfg_t cfg  = h->autofan_cfg;
    float aux_raw             = h->aux_temp_raw;
    pthread_mutex_unlock(&h->lock);

    bool temp_ok = !isnan(s.die_c);
    int fan_duty = -1; // -1 = let existing duty stand

    if (!cfg.enabled) {
        // Autofan disabled: BB owns duty at manual_pct (clamped 0..100).
        // This ensures BB fully owns fan duty in both modes when the feature is compiled in.
        int pct = cfg.manual_pct;
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        fan_duty = pct;
        (void)temp_ok;
    } else if (!temp_ok) {
        // Autofan enabled but temp read failed → fail-safe: max cooling
        fan_duty = 100;
    } else {
        float die_target = cfg.die_target_c;
        float aux_target = cfg.aux_target_c;
        float new_min    = (float)cfg.min_pct;

        // Update die EMA (alpha=0.2, faithful to TM)
        pthread_mutex_lock(&h->lock);
        if (h->die_ema < 0.0f) {
            h->die_ema = s.die_c;
        } else {
            h->die_ema = 0.2f * s.die_c + 0.8f * h->die_ema;
        }
        float die_ema = h->die_ema;

        // Update aux EMA if valid reading available
        bool aux_valid = (aux_raw >= 0.0f) && (aux_target > 0.0f);
        if (aux_valid) {
            if (h->aux_ema < 0.0f) {
                h->aux_ema = aux_raw;
            } else {
                h->aux_ema = 0.2f * aux_raw + 0.8f * h->aux_ema;
            }
        }
        float aux_ema = h->aux_ema;

        // Refresh PID limits from config
        bb_fan_pid_set_output_limits(&h->pid, new_min, 100.0f);

        // Input-src selection: whichever sensor has larger (ema - target)/target ratio
        float die_ratio = (die_ema - die_target) / die_target;
        float aux_ratio = (aux_valid && aux_ema >= 0.0f)
                          ? (aux_ema - aux_target) / aux_target
                          : -1e9f;

        if (aux_ratio > die_ratio) {
            h->pid_input    = aux_ema;
            h->pid_setpoint = aux_target;
            h->tel_pid_input_src = "aux";
        } else {
            h->pid_input    = die_ema;
            h->pid_setpoint = die_target;
            h->tel_pid_input_src = "die";
        }
        h->tel_pid_input_c = h->pid_input;

        // Arm PID on first valid reading
        if (!h->pid_armed) {
            bb_fan_pid_set_mode(&h->pid, BB_FAN_PID_AUTOMATIC);
            h->pid_armed = true;
            bb_log_i(TAG, "autofan PID armed: temp=%.1f setpoint=%.1f (P:5.0 I:0.1 D:2.0)",
                     h->pid_input, h->pid_setpoint);
        }
        bb_fan_pid_compute(&h->pid);
        fan_duty = (int)h->pid_output;

        // Telemetry snapshot
        h->tel_die_ema = die_ema;
        h->tel_aux_ema = aux_ema;

        pthread_mutex_unlock(&h->lock);
    }

    if (fan_duty >= 0 && h->drv->set_duty_pct) {
        h->drv->set_duty_pct(h->state, fan_duty);
        s.duty_pct = fan_duty;
    }
#endif /* CONFIG_BB_FAN_AUTOFAN */

    BB_LOCKED_COPY(&h->lock, h->cache, s);

    return BB_OK;
}

void bb_fan_snapshot(bb_fan_handle_t h, bb_fan_snapshot_t *out)
{
    if (!out) return;
    if (!h || !h->drv) {
        out->rpm      = -1;
        out->duty_pct = -1;
        out->die_c    = NAN;
        out->board_c  = NAN;
        return;
    }
    BB_LOCKED_COPY(&h->lock, *out, h->cache);
}

bb_err_t bb_fan_set_duty_pct(bb_fan_handle_t h, int pct)
{
    if (!h || !h->drv) return BB_ERR_INVALID_STATE;
    if (!h->drv->set_duty_pct) return BB_ERR_UNSUPPORTED;
    return h->drv->set_duty_pct(h->state, pct);
}

int bb_fan_get_duty_pct(bb_fan_handle_t h)
{
    if (!h || !h->drv) return -1;
    bb_fan_snapshot_t s;
    BB_LOCKED_COPY(&h->lock, s, h->cache);
    return s.duty_pct;
}

const char *bb_fan_name(bb_fan_handle_t h)
{
    if (!h || !h->drv) return NULL;
    return h->drv->name;
}

void bb_fan_set_primary(bb_fan_handle_t h) { s_primary = h; }
bb_fan_handle_t bb_fan_primary(void)        { return s_primary; }

#ifdef CONFIG_BB_FAN_AUTOFAN

bb_err_t bb_fan_set_autofan(bb_fan_handle_t h, const bb_fan_autofan_cfg_t *cfg)
{
    if (!h || !cfg) return BB_ERR_INVALID_ARG;
    pthread_mutex_lock(&h->lock);
    h->autofan_cfg = *cfg;
    // Disarm PID when switching enabled state so it re-initializes cleanly
    if (!cfg->enabled) {
        h->pid_armed = false;
        bb_fan_pid_set_mode(&h->pid, BB_FAN_PID_MANUAL);
    }
    pthread_mutex_unlock(&h->lock);
    return BB_OK;
}

bb_err_t bb_fan_get_autofan_cfg(bb_fan_handle_t h, bb_fan_autofan_cfg_t *out)
{
    if (!h || !out) return BB_ERR_INVALID_ARG;
    BB_LOCKED_COPY(&h->lock, *out, h->autofan_cfg);
    return BB_OK;
}

void bb_fan_autofan_set_clock(bb_fan_handle_t h, unsigned long (*fn)(void))
{
    if (!h) return;
    pthread_mutex_lock(&h->lock);
    bb_fan_pid_set_clock(&h->pid, fn);
    // Seed lastTime so sample gate fires promptly on first poll
    if (fn) {
        h->pid.lastTime = fn() - h->pid.sampleTime;
    }
    pthread_mutex_unlock(&h->lock);
}

bb_err_t bb_fan_set_aux_temp(bb_fan_handle_t h, float aux_c)
{
    if (!h) return BB_ERR_INVALID_ARG;
    BB_LOCKED_COPY(&h->lock, h->aux_temp_raw, aux_c);
    return BB_OK;
}

void bb_fan_get_autofan_telemetry(bb_fan_handle_t h, bb_fan_autofan_telemetry_t *out)
{
    if (!out) return;
    if (!h) {
        out->die_ema_c      = -1.0f;
        out->aux_ema_c      = -1.0f;
        out->pid_input_c    = -1.0f;
        out->pid_input_src  = "";
        return;
    }
    pthread_mutex_lock(&h->lock);
    out->die_ema_c     = h->tel_die_ema;
    out->aux_ema_c     = h->tel_aux_ema;
    out->pid_input_c   = h->tel_pid_input_c;
    out->pid_input_src = h->tel_pid_input_src;
    pthread_mutex_unlock(&h->lock);
}

#endif /* CONFIG_BB_FAN_AUTOFAN */

// ---------------------------------------------------------------------------
// JSON serializer — single builder shared by REST + bb_pub emitters.
// ---------------------------------------------------------------------------

#ifndef CONFIG_BB_FAN_AUTOFAN
void bb_fan_emit(bb_json_t obj, const bb_fan_snapshot_t *snap)
#else
void bb_fan_emit(bb_json_t obj, const bb_fan_snapshot_t *snap,
                 const bb_fan_autofan_telemetry_t *tel)
#endif
{
    if (!obj || !snap) return;

    if (snap->rpm >= 0) {
        bb_json_obj_set_number(obj, "rpm", (double)snap->rpm);
    } else {
        bb_json_obj_set_null(obj, "rpm");
    }

    if (snap->duty_pct >= 0) {
        bb_json_obj_set_number(obj, "duty_pct", (double)snap->duty_pct);
    } else {
        bb_json_obj_set_null(obj, "duty_pct");
    }

    if (!isnan(snap->die_c)) {
        bb_json_obj_set_number(obj, "die_c", (double)snap->die_c);
    } else {
        bb_json_obj_set_null(obj, "die_c");
    }

    if (!isnan(snap->board_c)) {
        bb_json_obj_set_number(obj, "board_c", (double)snap->board_c);
    } else {
        bb_json_obj_set_null(obj, "board_c");
    }

#ifdef CONFIG_BB_FAN_AUTOFAN
    if (tel) {
        if (tel->die_ema_c >= 0.0f) {
            bb_json_obj_set_number(obj, "die_ema_c", (double)tel->die_ema_c);
        } else {
            bb_json_obj_set_null(obj, "die_ema_c");
        }
        if (tel->aux_ema_c >= 0.0f) {
            bb_json_obj_set_number(obj, "vr_ema_c", (double)tel->aux_ema_c);
        } else {
            bb_json_obj_set_null(obj, "vr_ema_c");
        }
        if (tel->pid_input_c >= 0.0f) {
            bb_json_obj_set_number(obj, "pid_input_c", (double)tel->pid_input_c);
        } else {
            bb_json_obj_set_null(obj, "pid_input_c");
        }
        const char *src = tel->pid_input_src ? tel->pid_input_src : "";
        if (src[0] == 'a') src = "vr";
        bb_json_obj_set_string(obj, "pid_input_src", src);
    }
#endif
}

#ifdef BB_FAN_TESTING
#include "bb_fan_test.h"
void bb_fan_test_reset(void)
{
    s_primary = NULL;
}

#ifdef CONFIG_BB_FAN_AUTOFAN
void bb_fan_pid_set_mock_clock(bb_fan_handle_t h, unsigned long (*fn)(void))
{
    if (!h) return;
    bb_fan_pid_set_clock(&h->pid, fn);
    // Reset PID timer so sample-time gate fires immediately on next compute
    if (fn) {
        h->pid.lastTime = fn() - h->pid.sampleTime;
    }
}
#endif /* CONFIG_BB_FAN_AUTOFAN */

#endif /* BB_FAN_TESTING */

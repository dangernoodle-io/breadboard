// bb_fan_pid — PID controller for bb_fan autofan.
// Adapted from Brett Beauregard's Arduino PID Library (MIT).
// Default clock returns 0 (like TM's pid.c); host tests inject via bb_fan_pid_set_clock().

#include "bb_fan_pid.h"
#include <stddef.h>

/* Default clock — returns 0 so the PID gate is based on injected mock on host.
 * On ESP-IDF, bb_fan_autofan_inject_clock() replaces this with esp_timer. */
static unsigned long s_default_now_ms(void)
{
    return 0;
}

void bb_fan_pid_init(bb_fan_pid_t *pid, float *input, float *output, float *setpoint,
                     float Kp, float Ki, float Kd,
                     bb_fan_pid_p_mode_t POn, bb_fan_pid_direction_t dir)
{
    pid->input    = input;
    pid->output   = output;
    pid->setpoint = setpoint;
    pid->inAuto   = false;
    pid->now_ms   = s_default_now_ms;

    bb_fan_pid_set_output_limits(pid, 0, 255);
    pid->sampleTime = 100;

    bb_fan_pid_set_direction(pid, dir);
    bb_fan_pid_set_tunings_adv(pid, Kp, Ki, Kd, POn);

    pid->lastTime = pid->now_ms() - pid->sampleTime;
}

void bb_fan_pid_set_clock(bb_fan_pid_t *pid, bb_fan_pid_now_ms_fn fn)
{
    if (fn != NULL) {
        pid->now_ms = fn;
    }
}

void bb_fan_pid_set_mode(bb_fan_pid_t *pid, int mode)
{
    bool newAuto = (mode == BB_FAN_PID_AUTOMATIC);
    if (newAuto && !pid->inAuto) {
        bb_fan_pid_initialize(pid);
    }
    pid->inAuto = newAuto;
}

bool bb_fan_pid_compute(bb_fan_pid_t *pid)
{
    if (!pid->inAuto) return false;

    unsigned long now        = pid->now_ms();
    unsigned long timeChange = now - pid->lastTime;

    if (timeChange >= pid->sampleTime) {
        float input  = *(pid->input);
        float error  = *(pid->setpoint) - input;
        float dInput = input - pid->lastInput;
        pid->outputSum += pid->ki * error;

        if (!pid->pOnE) pid->outputSum -= pid->kp * dInput;

        if (pid->outputSum > pid->outMax) pid->outputSum = pid->outMax;
        else if (pid->outputSum < pid->outMin) pid->outputSum = pid->outMin;

        float output = pid->pOnE ? pid->kp * error : 0;
        output += pid->outputSum - pid->kd * dInput;

        if (output > pid->outMax) {
            pid->outputSum -= output - pid->outMax;
            output = pid->outMax;
        } else if (output < pid->outMin) {
            pid->outputSum += pid->outMin - output;
            output = pid->outMin;
        }

        *(pid->output) = output;
        pid->lastInput = input;
        pid->lastTime  = now;
        return true;
    }
    return false;
}

void bb_fan_pid_set_tunings_adv(bb_fan_pid_t *pid, float Kp, float Ki, float Kd,
                                 bb_fan_pid_p_mode_t POn)
{
    if (Kp < 0 || Ki < 0 || Kd < 0) return;

    pid->pOn  = POn;
    pid->pOnE = (POn == BB_FAN_PID_P_ON_E);

    pid->dispKp = Kp;
    pid->dispKi = Ki;
    pid->dispKd = Kd;

    float sampleTimeInSec = ((float)pid->sampleTime) / 1000.0f;
    pid->kp = Kp;
    pid->ki = Ki * sampleTimeInSec;
    pid->kd = Kd / sampleTimeInSec;

    if (pid->controllerDirection == BB_FAN_PID_REVERSE) {
        pid->kp = -pid->kp;
        pid->ki = -pid->ki;
        pid->kd = -pid->kd;
    }
}

void bb_fan_pid_set_tunings(bb_fan_pid_t *pid, float Kp, float Ki, float Kd)
{
    bb_fan_pid_set_tunings_adv(pid, Kp, Ki, Kd, pid->pOn);
}

void bb_fan_pid_set_sample_time(bb_fan_pid_t *pid, int newSampleTime)
{
    if (newSampleTime > 0) {
        float ratio  = (float)newSampleTime / (float)pid->sampleTime;
        pid->ki     *= ratio;
        pid->kd     /= ratio;
        pid->sampleTime = (unsigned long)newSampleTime;
    }
}

void bb_fan_pid_set_output_limits(bb_fan_pid_t *pid, float min, float max)
{
    if (min >= max) return;
    pid->outMin = min;
    pid->outMax = max;

    if (pid->inAuto) {
        if (*(pid->output) > max) *(pid->output) = max;
        else if (*(pid->output) < min) *(pid->output) = min;

        if (pid->outputSum > max) pid->outputSum = max;
        else if (pid->outputSum < min) pid->outputSum = min;
    }
}

void bb_fan_pid_set_direction(bb_fan_pid_t *pid, bb_fan_pid_direction_t dir)
{
    if (pid->inAuto && dir != pid->controllerDirection) {
        pid->kp = -pid->kp;
        pid->ki = -pid->ki;
        pid->kd = -pid->kd;
    }
    pid->controllerDirection = dir;
}

void bb_fan_pid_initialize(bb_fan_pid_t *pid)
{
    pid->outputSum = *(pid->output);
    pid->lastInput = *(pid->input);
    if (pid->outputSum > pid->outMax) pid->outputSum = pid->outMax;
    else if (pid->outputSum < pid->outMin) pid->outputSum = pid->outMin;
}

int bb_fan_pid_get_mode(bb_fan_pid_t *pid)
{
    return pid->inAuto ? BB_FAN_PID_AUTOMATIC : BB_FAN_PID_MANUAL;
}

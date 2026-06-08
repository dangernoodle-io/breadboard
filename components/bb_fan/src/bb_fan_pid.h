// bb_fan_pid — PID controller for bb_fan autofan.
// Adapted from Brett Beauregard's Arduino PID Library (MIT).
// Time source is injectable for host testability (bb_clock_now_ms by default).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BB_FAN_PID_AUTOMATIC 1
#define BB_FAN_PID_MANUAL    0

typedef enum {
    BB_FAN_PID_DIRECT  = 0,
    BB_FAN_PID_REVERSE = 1,
} bb_fan_pid_direction_t;

typedef enum {
    BB_FAN_PID_P_ON_M = 0,
    BB_FAN_PID_P_ON_E = 1,
} bb_fan_pid_p_mode_t;

typedef unsigned long (*bb_fan_pid_now_ms_fn)(void);

typedef struct {
    float dispKp;
    float dispKi;
    float dispKd;

    float kp;
    float ki;
    float kd;

    bb_fan_pid_direction_t controllerDirection;
    bb_fan_pid_p_mode_t    pOn;
    bool                   pOnE;

    float *input;
    float *output;
    float *setpoint;

    unsigned long lastTime;
    unsigned long sampleTime;
    float         outMin;
    float         outMax;
    bool          inAuto;

    float outputSum;
    float lastInput;

    bb_fan_pid_now_ms_fn now_ms;
} bb_fan_pid_t;

void bb_fan_pid_init(bb_fan_pid_t *pid, float *input, float *output, float *setpoint,
                     float Kp, float Ki, float Kd,
                     bb_fan_pid_p_mode_t POn, bb_fan_pid_direction_t dir);

void bb_fan_pid_set_clock(bb_fan_pid_t *pid, bb_fan_pid_now_ms_fn fn);
void bb_fan_pid_set_mode(bb_fan_pid_t *pid, int mode);
bool bb_fan_pid_compute(bb_fan_pid_t *pid);
void bb_fan_pid_set_output_limits(bb_fan_pid_t *pid, float min, float max);
void bb_fan_pid_set_tunings(bb_fan_pid_t *pid, float Kp, float Ki, float Kd);
void bb_fan_pid_set_tunings_adv(bb_fan_pid_t *pid, float Kp, float Ki, float Kd,
                                 bb_fan_pid_p_mode_t POn);
void bb_fan_pid_set_sample_time(bb_fan_pid_t *pid, int newSampleTime);
void bb_fan_pid_set_direction(bb_fan_pid_t *pid, bb_fan_pid_direction_t dir);
void bb_fan_pid_initialize(bb_fan_pid_t *pid);
int  bb_fan_pid_get_mode(bb_fan_pid_t *pid);

#ifdef __cplusplus
}
#endif
